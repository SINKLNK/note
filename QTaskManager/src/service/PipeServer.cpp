#include "PipeServer.h"
#include <sddl.h>
namespace monitoring
{
namespace
{
bool interactivePeer(HANDLE pipe)
{
    if (!ImpersonateNamedPipeClient(pipe))
        return false;
    HANDLE raw = nullptr;
    bool ok = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw) != FALSE;
    WinHandle token(raw);
    bool allowed = false;
    if (ok)
        for (auto type : {WinInteractiveSid, WinLocalSystemSid, WinBuiltinAdministratorsSid})
        {
            BYTE sid[SECURITY_MAX_SID_SIZE];
            DWORD size = sizeof(sid);
            BOOL member = FALSE;
            if (CreateWellKnownSid(type, nullptr, sid, &size) &&
                CheckTokenMembership(token.get(), sid, &member) && member)
                allowed = true;
        }
    if (!RevertToSelf())
        std::terminate();
    return allowed;
}
} // namespace
PipeServer::PipeServer(ClientLeases &leases, std::wstring name, std::function<bool(HANDLE)> identity,
                       std::wstring acl)
    : leases_(leases), name_(std::move(name)), identity_(identity ? std::move(identity) : interactivePeer),
      acl_(std::move(acl))
{
}
PipeServer::~PipeServer()
{
    stop();
}
SourceStatus PipeServer::start(std::function<Frame(const Frame &)> handler)
{
    if (!pipes_.empty())
        return {SourceState::Failed, ERROR_ALREADY_EXISTS, "Pipe already started"};
    stop_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop_)
        return {SourceState::Failed, GetLastError(), "Create stop event"};
    PSECURITY_DESCRIPTOR sd = nullptr;
    // 0x12019b grants read/write/synchronize, deliberately excludes FILE_CREATE_PIPE_INSTANCE (4).
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl_.c_str(), SDDL_REVISION_1, &sd, nullptr))
        return {SourceState::Failed, GetLastError(), "Pipe ACL"};
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    DWORD error = 0;
    for (unsigned i = 0; i < 8; ++i)
    {
        WinHandle pipe(CreateNamedPipeW(
            name_.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (i == 0 ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 8, 65536, 65536, 0,
            &sa));
        if (!pipe)
        {
            error = GetLastError();
            break;
        }
        pipes_.push_back(std::move(pipe));
    }
    LocalFree(sd);
    if (error)
    {
        stop();
        return {SourceState::Failed, error, "Cannot own local pipe"};
    }
    handler_ = std::move(handler);
    for (size_t i = 0; i < pipes_.size(); ++i)
        threads_.emplace_back([this, i] { serve(pipes_[i].get(), i + 1); });
    return {SourceState::Ready};
}
void PipeServer::serve(HANDLE pipe, uint64_t id)
{
    while (WaitForSingleObject(stop_.get(), 0) != WAIT_OBJECT_0)
    {
        if (!awaitPipeConnect(pipe, stop_.get()))
            break;
        Frame request;
        bool live = readFrame(pipe, stop_.get(), request, 5000) && request.kind == MessageKind::Hello &&
                    request.payload.empty() && identity_(pipe);
        if (live)
        {
            leases_.connected(id, GetTickCount64());
            live = writeFrame(pipe, stop_.get(), handler_(request), 2000);
        }
        while (live)
        {
            if (!readFrame(pipe, stop_.get(), request, 15000) || !request.payload.empty())
                break;
            if (request.kind != MessageKind::Heartbeat && request.kind != MessageKind::GetStatus &&
                request.kind != MessageKind::GetSnapshot)
                break;
            leases_.heartbeat(id, GetTickCount64());
            Frame response;
            if (request.kind == MessageKind::GetSnapshot)
            {
                std::shared_ptr<const Snapshot> latest;
                {
                    std::lock_guard lock(latestMutex_);
                    latest = latest_;
                }
                response = {latest ? MessageKind::SnapshotReply : MessageKind::Error, request.requestId,
                            latest ? encodeSnapshot(*latest) : std::vector<std::byte>{}};
            }
            else
                response = handler_(request);
            live = writeFrame(pipe, stop_.get(), response, 2000);
        }
        leases_.disconnected(id, GetTickCount64());
        DisconnectNamedPipe(pipe);
    }
}
void PipeServer::publish(std::shared_ptr<const Snapshot> snapshot)
{
    std::lock_guard lock(latestMutex_);
    latest_ = std::move(snapshot);
}
void PipeServer::stop()
{
    if (stop_)
        SetEvent(stop_.get());
    for (auto &t : threads_)
        if (t.joinable())
            t.join();
    threads_.clear();
    pipes_.clear();
    stop_.reset();
    publish(nullptr);
}
} // namespace monitoring
