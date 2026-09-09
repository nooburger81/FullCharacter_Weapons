// =============================================================================
// Claude Assistant Plugin for Unreal Engine 5
// Copyright (c) 2026 PixelsDesign - Luca Tocco. All Rights Reserved.
// =============================================================================

// ClaudeMcpServer.cpp

#include "ClaudeMcpServer.h"
#include "ClaudeToolRegistry.h"

#include "HttpServerModule.h"
#include "HttpServerResponse.h"
#include "HttpServerRequest.h"
#include "HttpPath.h"
#include "IHttpRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Async/Async.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Misc/Guid.h"

DEFINE_LOG_CATEGORY_STATIC(LogClaudeMcp, Log, All);

#define CLAUDE_MCP_PROTOCOL_VERSION TEXT("2024-11-05")
#define CLAUDE_MCP_SERVER_NAME      TEXT("unreal-editor")
#define CLAUDE_MCP_SERVER_VERSION   TEXT("1.0.0")

namespace
{
	FString ReadBodyAsString(const FHttpServerRequest& Req)
	{
		if (Req.Body.Num() == 0)
		{
			return FString();
		}
		TArray<uint8> Buf = Req.Body;
		Buf.Add(0);
		return FString(UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(Buf.GetData())));
	}

	FString McpSerializeJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	TUniquePtr<FHttpServerResponse> MakeJsonResponse(const TSharedRef<FJsonObject>& Body, int32 Code = 200)
	{
		const FString Serialized = McpSerializeJson(Body);
		TUniquePtr<FHttpServerResponse> Resp = FHttpServerResponse::Create(Serialized, TEXT("application/json"));
		Resp->Code = static_cast<EHttpServerResponseCodes>(Code);
		return Resp;
	}

	TSharedRef<FJsonObject> MakeRpcResult(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Result)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		if (Id.IsValid())
		{
			Obj->SetField(TEXT("id"), Id);
		}
		Obj->SetObjectField(TEXT("result"), Result);
		return Obj;
	}

	TSharedRef<FJsonObject> MakeRpcError(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Err = MakeShared<FJsonObject>();
		Err->SetNumberField(TEXT("code"), Code);
		Err->SetStringField(TEXT("message"), Message);

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		if (Id.IsValid())
		{
			Obj->SetField(TEXT("id"), Id);
		}
		Obj->SetObjectField(TEXT("error"), Err);
		return Obj;
	}

	TSharedRef<FJsonObject> MakeInitializeResult()
	{
		TSharedRef<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
		ServerInfo->SetStringField(TEXT("name"), CLAUDE_MCP_SERVER_NAME);
		ServerInfo->SetStringField(TEXT("version"), CLAUDE_MCP_SERVER_VERSION);

		TSharedRef<FJsonObject> ToolsCap = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Caps = MakeShared<FJsonObject>();
		Caps->SetObjectField(TEXT("tools"), ToolsCap);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("protocolVersion"), CLAUDE_MCP_PROTOCOL_VERSION);
		Result->SetObjectField(TEXT("capabilities"), Caps);
		Result->SetObjectField(TEXT("serverInfo"), ServerInfo);
		return Result;
	}

	TSharedRef<FJsonObject> MakeToolsListResult()
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("tools"), FClaudeToolRegistry::Get().GetToolSchemasJson());
		return Result;
	}

	/** Wrap a tool's text output as an MCP tool result (content blocks). */
	TSharedRef<FJsonObject> MakeToolCallResult(const FString& Text, bool bIsError = false)
	{
		TSharedRef<FJsonObject> TextBlock = MakeShared<FJsonObject>();
		TextBlock->SetStringField(TEXT("type"), TEXT("text"));
		TextBlock->SetStringField(TEXT("text"), Text);

		TArray<TSharedPtr<FJsonValue>> Content = { MakeShared<FJsonValueObject>(TextBlock) };

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("content"), Content);
		if (bIsError)
		{
			Result->SetBoolField(TEXT("isError"), true);
		}
		return Result;
	}
}

FClaudeMcpServer& FClaudeMcpServer::Get()
{
	static FClaudeMcpServer Instance;
	return Instance;
}

bool FClaudeMcpServer::Start(int32 InPort)
{
	if (bRunning)
	{
		return true;
	}

	if (AuthToken.IsEmpty())
	{
		AuthToken = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}

	FHttpServerModule& Module = FHttpServerModule::Get();
	Router = Module.GetHttpRouter(InPort);
	if (!Router.IsValid())
	{
		UE_LOG(LogClaudeMcp, Error, TEXT("MCP: failed to get HTTP router on port %d"), InPort);
		return false;
	}

	Route = Router->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_POST,
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
		FHttpRequestHandler::CreateLambda(
			[this](const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
			{
				return this->HandleMcp(Req, OnComplete);
			})
#else
		[this](const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
		{
			return this->HandleMcp(Req, OnComplete);
		}
#endif
	);

	Module.StartAllListeners();
	Port = InPort;
	bRunning = true;

	UE_LOG(LogClaudeMcp, Log, TEXT("MCP server listening on http://127.0.0.1:%d/mcp (tools=%d)"),
		Port, FClaudeToolRegistry::Get().Num());
	return true;
}

void FClaudeMcpServer::Stop()
{
	if (!bRunning)
	{
		return;
	}
	if (Router.IsValid() && Route.IsValid())
	{
		Router->UnbindRoute(Route);
	}
	Route.Reset();
	Router.Reset();
	bRunning = false;
	UE_LOG(LogClaudeMcp, Log, TEXT("MCP server stopped."));
}

bool FClaudeMcpServer::HandleMcp(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	// Defense-in-depth vs DNS-rebinding: the HTTP listener binds all interfaces, so only accept
	// requests whose Host header addresses localhost (complements the token check below).
	for (const TPair<FString, TArray<FString>>& Header : Req.Headers)
	{
		if (Header.Key.Equals(TEXT("Host"), ESearchCase::IgnoreCase) && Header.Value.Num() > 0)
		{
			const FString& Host = Header.Value[0];
			if (!Host.StartsWith(TEXT("127.0.0.1")) && !Host.StartsWith(TEXT("localhost")) && !Host.StartsWith(TEXT("[::1]")))
			{
				TUniquePtr<FHttpServerResponse> Denied = FHttpServerResponse::Create(FString(TEXT("{\"error\":\"forbidden host\"}")), TEXT("application/json"));
				Denied->Code = EHttpServerResponseCodes::Denied;
				OnComplete(MoveTemp(Denied));
				return true;
			}
			break;
		}
	}

	// Auth: require the per-session token embedded in the CLI's mcp-config headers. A browser
	// doing DNS-rebinding / blind-CSRF cannot read that file, so it cannot supply the token.
	if (!AuthToken.IsEmpty())
	{
		FString Provided;
		for (const TPair<FString, TArray<FString>>& Header : Req.Headers)
		{
			if (Header.Key.Equals(TEXT("X-Claude-Assistant-Token"), ESearchCase::IgnoreCase) && Header.Value.Num() > 0)
			{
				Provided = Header.Value[0];
				break;
			}
		}
		if (Provided != AuthToken)
		{
			TUniquePtr<FHttpServerResponse> Denied = FHttpServerResponse::Create(FString(TEXT("{\"error\":\"unauthorized\"}")), TEXT("application/json"));
			Denied->Code = EHttpServerResponseCodes::Denied;
			OnComplete(MoveTemp(Denied));
			return true;
		}
	}

	const FString BodyStr = ReadBodyAsString(Req);

	TSharedPtr<FJsonObject> ReqJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyStr);
	if (!FJsonSerializer::Deserialize(Reader, ReqJson) || !ReqJson.IsValid())
	{
		OnComplete(MakeJsonResponse(MakeRpcError(nullptr, -32700, TEXT("Parse error")), 400));
		return true;
	}

	FString Method;
	ReqJson->TryGetStringField(TEXT("method"), Method);
	TSharedPtr<FJsonValue> IdVal = ReqJson->TryGetField(TEXT("id"));

	UE_LOG(LogClaudeMcp, Log, TEXT("MCP <- method=%s"), *Method);

	if (Method == TEXT("initialize"))
	{
		OnComplete(MakeJsonResponse(MakeRpcResult(IdVal, MakeInitializeResult())));
		return true;
	}

	if (Method == TEXT("notifications/initialized"))
	{
		TUniquePtr<FHttpServerResponse> Resp = FHttpServerResponse::Create(FString(), TEXT("application/json"));
		Resp->Code = EHttpServerResponseCodes::NoContent;
		OnComplete(MoveTemp(Resp));
		return true;
	}

	if (Method == TEXT("tools/list"))
	{
		OnComplete(MakeJsonResponse(MakeRpcResult(IdVal, MakeToolsListResult())));
		return true;
	}

	if (Method == TEXT("tools/call"))
	{
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		if (!ReqJson->TryGetObjectField(TEXT("params"), ParamsPtr) || !ParamsPtr)
		{
			OnComplete(MakeJsonResponse(MakeRpcError(IdVal, -32602, TEXT("Missing params"))));
			return true;
		}
		const TSharedPtr<FJsonObject> Params = *ParamsPtr;

		FString ToolName;
		Params->TryGetStringField(TEXT("name"), ToolName);

		UE_LOG(LogClaudeMcp, Log, TEXT("MCP tools/call name=%s"), *ToolName);

		TSharedPtr<FJsonObject> Args;
		const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
		if (Params->TryGetObjectField(TEXT("arguments"), ArgsPtr) && ArgsPtr)
		{
			Args = *ArgsPtr;
		}
		else
		{
			Args = MakeShared<FJsonObject>();
		}

		// Editor mutations must run on the game thread. Keep the HTTP response
		// pending until the tool finishes, then complete it with the result.
		AsyncTask(ENamedThreads::GameThread, [ToolName, Args, IdVal, OnComplete]()
		{
			const FClaudeToolResult ToolResult = FClaudeToolRegistry::Get().ExecuteTool(ToolName, Args);
			OnComplete(MakeJsonResponse(MakeRpcResult(IdVal, MakeToolCallResult(ToolResult.Content, !ToolResult.bSuccess))));
		});
		return true;
	}

	OnComplete(MakeJsonResponse(MakeRpcError(IdVal, -32601, FString::Printf(TEXT("Method not found: %s"), *Method))));
	return true;
}
