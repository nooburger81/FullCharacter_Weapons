// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeCliSession.cpp

#include "ClaudeCliSession.h"
#include "ClaudeAssistantSettings.h"
#include "ClaudeToolRegistry.h"
#include "ClaudeMcpServer.h"

#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Async/Async.h"

DEFINE_LOG_CATEGORY_STATIC(LogClaudeCli, Log, All);

// ============================================================================
// Background thread that reads newline-delimited JSON (JSONL) from claude stdout.
// ============================================================================
class FClaudeCliStdoutReader : public FRunnable
{
public:
	FClaudeCliStdoutReader(void* InPipe, TWeakPtr<FClaudeCliSession> InOwner, FProcHandle InProc)
		: Pipe(InPipe), Owner(InOwner), Proc(InProc) {}

	virtual bool Init() override { return true; }

	virtual uint32 Run() override
	{
		TArray<uint8> ByteBuffer;
		FString ExitReason = TEXT("stopped");

		while (!bShouldStop)
		{
			// Read raw bytes: decoding per-chunk would corrupt UTF-8 code points that
			// straddle a read boundary (mojibake on accented text).
			TArray<uint8> Chunk;
			const bool bGotData = FPlatformProcess::ReadPipeToArray(Pipe, Chunk) && Chunk.Num() > 0;
			if (bGotData)
			{
				ByteBuffer.Append(Chunk);
			}

			// Emit every complete (newline-terminated) JSONL line, decoding whole lines.
			int32 NewlineIdx = INDEX_NONE;
			while (ByteBuffer.Find((uint8)'\n', NewlineIdx))
			{
				TArray<uint8> LineBytes;
				LineBytes.Append(ByteBuffer.GetData(), NewlineIdx);
				LineBytes.Add(0);
				ByteBuffer.RemoveAt(0, NewlineIdx + 1);

				FString Line = FString(UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(LineBytes.GetData())));
				Line.TrimStartAndEndInline();
				if (Line.IsEmpty())
				{
					continue;
				}

				TSharedPtr<FJsonObject> Obj;
				TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(Line);
				if (FJsonSerializer::Deserialize(JsonReader, Obj) && Obj.IsValid())
				{
					TWeakPtr<FClaudeCliSession> Weak = Owner;
					TSharedPtr<FJsonObject> Captured = Obj;
					AsyncTask(ENamedThreads::GameThread, [Weak, Captured]()
					{
						if (TSharedPtr<FClaudeCliSession> Pinned = Weak.Pin())
						{
							Pinned->HandleStreamMessage(Captured);
						}
					});
				}
				else
				{
					UE_LOG(LogClaudeCli, Verbose, TEXT("reader: non-JSON line: %s"), *Line);
				}
			}

			if (!bGotData)
			{
				// No data available: detect child exit / closed pipe so we don't spin
				// forever (and so a mid-turn CLI death still ends the turn cleanly).
				if (Proc.IsValid() && !FPlatformProcess::IsProcRunning(Proc))
				{
					ExitReason = TEXT("process_exited");
					break;
				}
				FPlatformProcess::Sleep(0.02f);
			}
		}

		TWeakPtr<FClaudeCliSession> Weak = Owner;
		const FString Reason = ExitReason;
		AsyncTask(ENamedThreads::GameThread, [Weak, Reason]()
		{
			if (TSharedPtr<FClaudeCliSession> Pinned = Weak.Pin())
			{
				Pinned->HandleReaderExit(Reason);
			}
		});
		return 0;
	}

	virtual void Stop() override { bShouldStop = true; }

private:
	void* Pipe;
	TWeakPtr<FClaudeCliSession> Owner;
	FProcHandle Proc;
	FThreadSafeBool bShouldStop = false;
};

// ============================================================================
// Helpers
// ============================================================================
namespace
{
	/** Write {"mcpServers":{"unreal-editor":{"type":"http","url":".../mcp"}}} and return its path. */
	FString WriteMcpConfig(int32 Port)
	{
		TSharedRef<FJsonObject> Server = MakeShared<FJsonObject>();
		Server->SetStringField(TEXT("type"), TEXT("http"));
		Server->SetStringField(TEXT("url"), FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), Port));

		// Per-session auth token so only our CLI (which holds this file) can call the tools.
		const FString Token = FClaudeMcpServer::Get().GetAuthToken();
		if (!Token.IsEmpty())
		{
			TSharedRef<FJsonObject> Headers = MakeShared<FJsonObject>();
			Headers->SetStringField(TEXT("X-Claude-Assistant-Token"), Token);
			Server->SetObjectField(TEXT("headers"), Headers);
		}

		TSharedRef<FJsonObject> Servers = MakeShared<FJsonObject>();
		Servers->SetObjectField(TEXT("unreal-editor"), Server);

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("mcpServers"), Servers);

		FString Serialized;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
		FJsonSerializer::Serialize(Root, Writer);

		const FString FilePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ClaudeAssistant"), TEXT("mcp-config.json"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		if (!FFileHelper::SaveStringToFile(Serialized, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogClaudeCli, Error, TEXT("failed to write mcp-config to %s"), *FilePath);
			return FString();
		}
		return FilePath;
	}
}

// ============================================================================
// FClaudeCliSession
// ============================================================================

FClaudeCliSession::~FClaudeCliSession()
{
	Stop();
}

bool FClaudeCliSession::IsRunning() const
{
	return ProcessHandle.IsValid() && FPlatformProcess::IsProcRunning(const_cast<FProcHandle&>(ProcessHandle));
}

FString FClaudeCliSession::BuildArgs(const FString& OptionalSystemPrompt) const
{
	const UClaudeAssistantSettings* Settings = GetDefault<UClaudeAssistantSettings>();

	TArray<FString> Parts;
	Parts.Add(TEXT("--input-format stream-json"));
	Parts.Add(TEXT("--output-format stream-json"));
	Parts.Add(TEXT("--verbose"));
	Parts.Add(TEXT("--print"));

	// Auto-approve so the agentic loop doesn't stall on permission prompts in headless mode,
	// but SCOPED to this plugin's MCP tools only — never a global permission bypass. Other
	// tools (Bash/Write/Edit/other MCP servers) keep Claude Code's normal gating.
	if (Settings && Settings->bAutoApproveMcpTools)
	{
		TArray<FString> Allowed;
		for (const FString& ToolName : FClaudeToolRegistry::Get().GetToolNames())
		{
			Allowed.Add(FString::Printf(TEXT("mcp__unreal-editor__%s"), *ToolName));
		}
		if (Allowed.Num() > 0)
		{
			Parts.Add(FString::Printf(TEXT("--allowedTools \"%s\""), *FString::Join(Allowed, TEXT(","))));
		}
	}

	const int32 McpPort = Settings ? Settings->McpServerPort : 7777;
	const FString McpConfigPath = WriteMcpConfig(McpPort);
	if (!McpConfigPath.IsEmpty())
	{
		Parts.Add(FString::Printf(TEXT("--mcp-config \"%s\""), *McpConfigPath));
		// Use ONLY our MCP server, not the user's global Claude Code MCP servers, so the
		// agent's tool surface is exactly our editor tools.
		Parts.Add(TEXT("--strict-mcp-config"));
	}

	if (!OptionalSystemPrompt.IsEmpty())
	{
		FString Escaped = OptionalSystemPrompt
			.Replace(TEXT("\\"), TEXT("\\\\"))
			.Replace(TEXT("\""), TEXT("\\\""))
			.Replace(TEXT("\r"), TEXT(""))
			.Replace(TEXT("\n"), TEXT(" "));
		Parts.Add(FString::Printf(TEXT("--append-system-prompt \"%s\""), *Escaped));
	}

	return FString::Join(Parts, TEXT(" "));
}

bool FClaudeCliSession::Start(const FString& OptionalSystemPrompt)
{
	if (IsRunning())
	{
		return true;
	}

	// A previous spawn may have exited on its own (crash, quota, /login expiry): tear it
	// down before re-spawning so we don't orphan the old reader thread or leak handles.
	if (ProcessHandle.IsValid() || Reader || ReaderThread)
	{
		Stop();
	}

	const UClaudeAssistantSettings* Settings = GetDefault<UClaudeAssistantSettings>();
	const FString ExePath = Settings ? Settings->GetEffectiveCLIPath() : FString();
	if (ExePath.IsEmpty())
	{
		OnError.ExecuteIfBound(TEXT("Claude Code CLI not found. Install it and set 'Claude CLI Path' in Project Settings > Plugins > Claude Assistant, then run 'claude /login' once."));
		return false;
	}

	const FString Args = BuildArgs(OptionalSystemPrompt);
	UE_LOG(LogClaudeCli, Log, TEXT("spawning '%s' %s"), *ExePath, *Args);

	// stdin: parent writes, child reads (write end parent-local).
	if (!FPlatformProcess::CreatePipe(StdInReadPipe, StdInWritePipe, /*bWritePipeLocal=*/true))
	{
		OnError.ExecuteIfBound(TEXT("Failed to create stdin pipe."));
		return false;
	}
	// stdout: child writes, parent reads (read end parent-local).
	if (!FPlatformProcess::CreatePipe(StdOutReadPipe, StdOutWritePipe, /*bWritePipeLocal=*/false))
	{
		OnError.ExecuteIfBound(TEXT("Failed to create stdout pipe."));
		ShutdownPipes();
		return false;
	}

	// Scrub ANTHROPIC_API_KEY so the CLI bills the Pro/Max subscription (OAuth), not API credits.
	const FString SavedAPIKey = FPlatformMisc::GetEnvironmentVariable(TEXT("ANTHROPIC_API_KEY"));
	if (!SavedAPIKey.IsEmpty())
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("ANTHROPIC_API_KEY"), TEXT(""));
	}

	// Project dir as cwd so Claude Code auto-loads any CLAUDE.md next to the .uproject.
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	uint32 PID = 0;
	ProcessHandle = FPlatformProcess::CreateProc(
		*ExePath, *Args,
		/*bLaunchDetached=*/false,
		/*bLaunchHidden=*/true,
		/*bLaunchReallyHidden=*/true,
		&PID, /*PriorityModifier=*/0,
		/*OptionalWorkingDirectory=*/*ProjectDir,
		/*PipeWriteChild (stdout)=*/StdOutWritePipe,
		/*PipeReadChild (stdin)=*/StdInReadPipe);

	if (!SavedAPIKey.IsEmpty())
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("ANTHROPIC_API_KEY"), *SavedAPIKey);
	}

	if (!ProcessHandle.IsValid())
	{
		OnError.ExecuteIfBound(FString::Printf(TEXT("Failed to launch Claude CLI at '%s'."), *ExePath));
		ShutdownPipes();
		return false;
	}

	// Close child-side handles in the parent (child inherited duplicates).
	FPlatformProcess::ClosePipe(StdInReadPipe, nullptr);   StdInReadPipe = nullptr;
	FPlatformProcess::ClosePipe(nullptr, StdOutWritePipe); StdOutWritePipe = nullptr;

	Reader = new FClaudeCliStdoutReader(StdOutReadPipe, AsShared(), ProcessHandle);
	ReaderThread = FRunnableThread::Create(Reader, TEXT("ClaudeCliReader"));

	UE_LOG(LogClaudeCli, Log, TEXT("claude pid=%u started (cwd=%s)"), PID, *ProjectDir);
	return true;
}

void FClaudeCliSession::Stop()
{
	// Signal the reader to stop, then kill the process so its stdout pipe closes.
	// KillTree: claude.exe is a Node launcher that spawns the real worker; kill the whole tree.
	if (Reader)
	{
		Reader->Stop();
	}
	if (ProcessHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcessHandle))
	{
		FPlatformProcess::TerminateProc(ProcessHandle, /*KillTree=*/true);
	}
	// Join the reader BEFORE closing the process handle: the reader holds a copy of it and
	// polls IsProcRunning() on it, so the handle must stay valid until the thread ends.
	if (ReaderThread)
	{
		ReaderThread->WaitForCompletion();
		delete ReaderThread;
		ReaderThread = nullptr;
	}
	if (Reader)
	{
		delete Reader;
		Reader = nullptr;
	}
	if (ProcessHandle.IsValid())
	{
		FPlatformProcess::CloseProc(ProcessHandle);
		ProcessHandle.Reset();
	}

	ShutdownPipes();
}

void FClaudeCliSession::ShutdownPipes()
{
	if (StdInWritePipe || StdInReadPipe)
	{
		FPlatformProcess::ClosePipe(StdInReadPipe, StdInWritePipe);
		StdInReadPipe = nullptr; StdInWritePipe = nullptr;
	}
	if (StdOutReadPipe || StdOutWritePipe)
	{
		FPlatformProcess::ClosePipe(StdOutReadPipe, StdOutWritePipe);
		StdOutReadPipe = nullptr; StdOutWritePipe = nullptr;
	}
}

void FClaudeCliSession::SendUserMessage(const FString& Text)
{
	if (!IsRunning() || !StdInWritePipe)
	{
		OnError.ExecuteIfBound(TEXT("Cannot send: Claude CLI session is not running."));
		return;
	}

	// {"type":"user","message":{"role":"user","content":[{"type":"text","text":"..."}]}}
	TSharedRef<FJsonObject> TextBlock = MakeShared<FJsonObject>();
	TextBlock->SetStringField(TEXT("type"), TEXT("text"));
	TextBlock->SetStringField(TEXT("text"), Text);

	TArray<TSharedPtr<FJsonValue>> Content = { MakeShared<FJsonValueObject>(TextBlock) };

	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("role"), TEXT("user"));
	Message->SetArrayField(TEXT("content"), Content);

	TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetStringField(TEXT("type"), TEXT("user"));
	Envelope->SetObjectField(TEXT("message"), Message);

	FString Serialized;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
	FJsonSerializer::Serialize(Envelope, Writer);
	Serialized.AppendChar(TEXT('\n'));

	if (!FPlatformProcess::WritePipe(StdInWritePipe, Serialized))
	{
		OnError.ExecuteIfBound(TEXT("Failed to write to Claude CLI stdin."));
	}
}

void FClaudeCliSession::HandleStreamMessage(const TSharedPtr<FJsonObject>& Obj)
{
	if (!Obj.IsValid())
	{
		return;
	}

	FString Type;
	Obj->TryGetStringField(TEXT("type"), Type);

	if (Type == TEXT("assistant"))
	{
		const TSharedPtr<FJsonObject>* MsgObj = nullptr;
		if (Obj->TryGetObjectField(TEXT("message"), MsgObj) && MsgObj && (*MsgObj).IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ContentArr = nullptr;
			if ((*MsgObj)->TryGetArrayField(TEXT("content"), ContentArr) && ContentArr)
			{
				FString CombinedText;
				for (const TSharedPtr<FJsonValue>& V : *ContentArr)
				{
					const TSharedPtr<FJsonObject> Block = V->AsObject();
					if (!Block.IsValid())
					{
						continue;
					}
					FString BType;
					Block->TryGetStringField(TEXT("type"), BType);

					if (BType == TEXT("text"))
					{
						FString Txt;
						Block->TryGetStringField(TEXT("text"), Txt);
						if (!CombinedText.IsEmpty())
						{
							CombinedText += TEXT("\n");
						}
						CombinedText += Txt;
					}
					else if (BType == TEXT("tool_use"))
					{
						FString ToolName;
						Block->TryGetStringField(TEXT("name"), ToolName);
						OnToolCall.ExecuteIfBound(ToolName);
					}
				}
				if (!CombinedText.IsEmpty())
				{
					OnAssistantText.ExecuteIfBound(CombinedText);
				}
			}
		}
	}
	else if (Type == TEXT("stream_event"))
	{
		const TSharedPtr<FJsonObject>* EventObj = nullptr;
		if (Obj->TryGetObjectField(TEXT("event"), EventObj) && EventObj && (*EventObj).IsValid())
		{
			FString EventType;
			(*EventObj)->TryGetStringField(TEXT("type"), EventType);

			if (EventType == TEXT("content_block_delta"))
			{
				const TSharedPtr<FJsonObject>* DeltaObj = nullptr;
				if ((*EventObj)->TryGetObjectField(TEXT("delta"), DeltaObj) && DeltaObj && (*DeltaObj).IsValid())
				{
					FString DeltaType;
					(*DeltaObj)->TryGetStringField(TEXT("type"), DeltaType);
					if (DeltaType == TEXT("text_delta"))
					{
						FString Chunk;
						(*DeltaObj)->TryGetStringField(TEXT("text"), Chunk);
						if (!Chunk.IsEmpty())
						{
							OnAssistantTextDelta.ExecuteIfBound(Chunk);
						}
					}
				}
			}
			else if (EventType == TEXT("message_stop"))
			{
				OnAssistantMessageEnd.ExecuteIfBound();
			}
		}
	}
	else if (Type == TEXT("result"))
	{
		OnTurnComplete.ExecuteIfBound();
	}
	else if (Type == TEXT("system") || Type == TEXT("user") || Type == TEXT("rate_limit_event"))
	{
		// Expected noise: system init, echoed tool_results (Claude Code runs its own tool loop),
		// rate-limit info. Nothing to surface.
		UE_LOG(LogClaudeCli, Verbose, TEXT("stream message type='%s' (ignored)"), *Type);
	}
	else
	{
		FString Raw;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Raw);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
		UE_LOG(LogClaudeCli, Warning, TEXT("UNHANDLED stream message type='%s' full=%s"), *Type, *Raw);
	}
}

void FClaudeCliSession::HandleReaderExit(const FString& Reason)
{
	UE_LOG(LogClaudeCli, Log, TEXT("reader exited (%s)."), *Reason);
	OnExited.ExecuteIfBound();
}
