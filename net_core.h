#pragma once
// ============================================================================
// net_core.h — IOCP를 stdexec sender로 감싸는 계층
//
// 기존 코드 대비 바뀐 점은 딱 하나, "버퍼를 누가 소유하는가"이다.
//
//   [기존] RecvSender가 512바이트 내부 버퍼에 받은 뒤,
//          완료 시점에 std::vector<char>를 새로 만들어 복사해서 넘겼다.
//          SendSender는 생성자에서 한 번, connect()에서 또 한 번 복사했다.
//          → 패킷 하나당 힙 할당이 최소 2회.
//
//   [현재] 호출자가 버퍼를 소유하고, sender는 포인터만 받는다.
//          → 힙 할당 0회. 완료 시 넘기는 건 성공 여부와 바이트 수뿐.
//
// [수명 규칙] 호출자는 sender가 완료될 때까지 버퍼를 살려둬야 한다.
//            co_await로 완료를 기다리는 동안 버퍼가 살아있으면 된다.
// ============================================================================

#include <WS2tcpip.h>
#include <MSWSock.h>
#include <windows.h>

#include <cstdint>
#include <utility>

#include <stdexec/execution.hpp>

// ----------------------------------------------------------------------------
// 모든 비동기 작업의 공통 머리
// WSAOVERLAPPED가 반드시 첫 멤버여야 한다 (offset 0 보장).
// io_dispatch_loop가 LPOVERLAPPED를 IoOpBase*로 되돌려 캐스팅하기 때문.
// ----------------------------------------------------------------------------
struct IoOpBase {
    WSAOVERLAPPED overlapped{};
    DWORD bytes = 0;
    bool success = false;
    void (*complete_fn)(IoOpBase*) noexcept = nullptr;

    void complete() noexcept { complete_fn(this); }
};

// ----------------------------------------------------------------------------
// RecvSender — 호출자가 준 버퍼에 직접 수신
// ----------------------------------------------------------------------------

struct RecvResult {
    bool ok;
    uint32_t bytes;
};

struct RecvSender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(RecvResult)>;

    SOCKET socket;
    char* buffer;      // 호출자 소유
    ULONG capacity;

    RecvSender(SOCKET s, char* buf, ULONG cap)
        : socket(s), buffer(buf), capacity(cap) {}

    template <typename Receiver>
    struct operation : IoOpBase {
        Receiver receiver;
        SOCKET socket;
        WSABUF wsabuf;

        operation(Receiver r, SOCKET s, char* buf, ULONG cap)
            : receiver(std::move(r)), socket(s) {
            wsabuf.buf = buf;
            wsabuf.len = cap;
            complete_fn = &complete_impl;
        }

        static void complete_impl(IoOpBase* base) noexcept {
            auto* self = static_cast<operation*>(base);
            stdexec::set_value(std::move(self->receiver),
                RecvResult{ self->success, static_cast<uint32_t>(self->bytes) });
        }

        void start() noexcept {
            DWORD flags = 0;
            int ret = WSARecv(socket, &wsabuf, 1, nullptr, &flags, &overlapped, nullptr);
            if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                success = false;
                bytes = 0;
                complete();
            }
        }
    };

    template <typename Receiver>
    auto connect(Receiver r) noexcept {
        return operation<Receiver>(std::move(r), socket, buffer, capacity);
    }
};

// ----------------------------------------------------------------------------
// SendSender — 호출자가 준 버퍼를 그대로 전송 (복사 없음)
// ----------------------------------------------------------------------------

struct SendSender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(bool)>;

    SOCKET socket;
    const char* data;   // 호출자 소유. 완료까지 유효해야 한다
    ULONG length;

    SendSender(SOCKET s, const char* buf, ULONG len)
        : socket(s), data(buf), length(len) {}

    template <typename Receiver>
    struct operation : IoOpBase {
        Receiver receiver;
        SOCKET socket;
        WSABUF wsabuf;

        operation(Receiver r, SOCKET s, const char* buf, ULONG len)
            : receiver(std::move(r)), socket(s) {
            // WSABUF::buf가 비const라 캐스팅이 필요하다. WSASend는 읽기만 한다.
            wsabuf.buf = const_cast<char*>(buf);
            wsabuf.len = len;
            complete_fn = &complete_impl;
        }

        static void complete_impl(IoOpBase* base) noexcept {
            auto* self = static_cast<operation*>(base);
            stdexec::set_value(std::move(self->receiver), self->success);
        }

        void start() noexcept {
            int ret = WSASend(socket, &wsabuf, 1, nullptr, 0, &overlapped, nullptr);
            if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                success = false;
                complete();
            }
        }
    };

    template <typename Receiver>
    auto connect(Receiver r) noexcept {
        return operation<Receiver>(std::move(r), socket, data, length);
    }
};

// ----------------------------------------------------------------------------
// AcceptSender
// ----------------------------------------------------------------------------

struct AcceptSender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(SOCKET)>;

    SOCKET listen_socket;
    explicit AcceptSender(SOCKET s) : listen_socket(s) {}

    template <typename Receiver>
    struct operation : IoOpBase {
        Receiver receiver;
        SOCKET listen_socket;
        SOCKET accepted_socket = INVALID_SOCKET;
        char addr_buf[(sizeof(SOCKADDR_IN) + 16) * 2]{};

        operation(Receiver r, SOCKET listener)
            : receiver(std::move(r)), listen_socket(listener) {
            accepted_socket =
                WSASocketW(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
            complete_fn = &complete_impl;
        }

        static void complete_impl(IoOpBase* base) noexcept {
            auto* self = static_cast<operation*>(base);
            SOCKET result = INVALID_SOCKET;
            if (self->success) {
                // AcceptEx로 받은 소켓은 리슨 소켓의 속성을 물려받지 않는다.
                // 이걸 빼먹으면 getpeername, shutdown 등이 실패한다.
                setsockopt(self->accepted_socket, SOL_SOCKET,
                    SO_UPDATE_ACCEPT_CONTEXT,
                    reinterpret_cast<char*>(&self->listen_socket),
                    sizeof(SOCKET));
                result = self->accepted_socket;
            }
            else if (self->accepted_socket != INVALID_SOCKET) {
                closesocket(self->accepted_socket);   // 실패 시 소켓 누수 방지
            }
            stdexec::set_value(std::move(self->receiver), result);
        }

        void start() noexcept {
            if (accepted_socket == INVALID_SOCKET) {
                success = false;
                complete();
                return;
            }
            DWORD bytes = 0;
            BOOL ret = AcceptEx(listen_socket, accepted_socket, addr_buf, 0,
                sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
                &bytes, &overlapped);
            if (!ret && WSAGetLastError() != WSA_IO_PENDING) {
                success = false;
                complete();
            }
        }
    };

    template <typename Receiver>
    auto connect(Receiver r) noexcept {
        return operation<Receiver>(std::move(r), listen_socket);
    }
};
