//===-- Socket.cpp ----------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Host/Socket.h"

#include "lldb/Core/Log.h"
#include "lldb/Core/RegularExpression.h"
#include "lldb/Host/Config.h"
#include "lldb/Host/Host.h"
#include "lldb/Host/SocketAddress.h"
#include "lldb/Host/StringConvert.h"
#include "lldb/Host/TimeValue.h"
#include "lldb/Host/common/TCPSocket.h"
#include "lldb/Host/common/UDPSocket.h"

#ifndef LLDB_DISABLE_POSIX
#include "lldb/Host/posix/DomainSocket.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

#ifdef __linux__
#include "lldb/Host/linux/AbstractSocket.h"
#endif

#ifdef __ANDROID_NDK__
#include <linux/tcp.h>
#include <bits/error_constants.h>
#include <asm-generic/errno-base.h>
#include <errno.h>
#include <arpa/inet.h>
#if defined(ANDROID_ARM_BUILD_STATIC) || defined(ANDROID_MIPS_BUILD_STATIC)
#include <unistd.h>
#include <sys/syscall.h>
#include <fcntl.h>
#endif // ANDROID_ARM_BUILD_STATIC || ANDROID_MIPS_BUILD_STATIC
#endif // __ANDROID_NDK__

using namespace lldb;
using namespace lldb_private;

#if defined(_WIN32)
typedef const char * set_socket_option_arg_type;
typedef char * get_socket_option_arg_type;
const NativeSocket Socket::kInvalidSocketValue = INVALID_SOCKET;
#else // #if defined(_WIN32)
typedef const void * set_socket_option_arg_type;
typedef void * get_socket_option_arg_type;
const NativeSocket Socket::kInvalidSocketValue = -1;
#endif // #if defined(_WIN32)

namespace {

bool IsInterrupted()
{
#if defined(_WIN32)
    return ::WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

}

Socket::Socket(NativeSocket socket, SocketProtocol protocol, bool should_close)
    : IOObject(eFDTypeSocket, should_close)
    , m_protocol(protocol)
    , m_socket(socket)
{

}

Socket::~Socket()
{
    Close();
}

std::unique_ptr<Socket> Socket::Create(const SocketProtocol protocol, bool child_processes_inherit, Error &error)
{
    error.Clear();

    std::unique_ptr<Socket> socket_up;
    switch (protocol)
    {
    case ProtocolTcp:
        socket_up.reset(new TCPSocket(child_processes_inherit, error));
        break;
    case ProtocolUdp:
        socket_up.reset(new UDPSocket(child_processes_inherit, error));
        break;
    case ProtocolUnixDomain:
#ifndef LLDB_DISABLE_POSIX
        socket_up.reset(new DomainSocket(child_processes_inherit, error));
#else
        error.SetErrorString("Unix domain sockets are not supported on this platform.");
#endif
        break;
    case ProtocolUnixAbstract:
#ifdef __linux__
        socket_up.reset(new AbstractSocket(child_processes_inherit, error));
#else
        error.SetErrorString("Abstract domain sockets are not supported on this platform.");
#endif
        break;
    }

    if (error.Fail())
        socket_up.reset();

    return socket_up;
}

Error Socket::TcpConnect(llvm::StringRef host_and_port, bool child_processes_inherit, Socket *&socket)
{
    Log *log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_COMMUNICATION));
    if (log)
        log->Printf ("Socket::%s (host/port = %s)", __FUNCTION__, host_and_port.data());

    Error error;
    std::unique_ptr<Socket> connect_socket(Create(ProtocolTcp, child_processes_inherit, error));
    if (error.Fail())
        return error;

    error = connect_socket->Connect(host_and_port);
    if (error.Success())
      socket = connect_socket.release();

    return error;
}

Error
Socket::TcpListen (llvm::StringRef host_and_port,
                   bool child_processes_inherit,
                   Socket *&socket,
                   Predicate<uint16_t>* predicate,
                   int backlog)
{
    Log *log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_CONNECTION));
    if (log)
        log->Printf ("Socket::%s (%s)", __FUNCTION__, host_and_port.data());

    Error error;
    std::string host_str;
    std::string port_str;
    int32_t port = INT32_MIN;
    if (!DecodeHostAndPort (host_and_port, host_str, port_str, port, &error))
        return error;

    std::unique_ptr<TCPSocket> listen_socket(new TCPSocket(child_processes_inherit, error));
    if (error.Fail())
        return error;

    error = listen_socket->Listen(host_and_port, backlog);
    if (error.Success())
    {
        // We were asked to listen on port zero which means we
        // must now read the actual port that was given to us
        // as port zero is a special code for "find an open port
        // for me".
        if (port == 0)
            port = listen_socket->GetLocalPortNumber();

        // Set the port predicate since when doing a listen://<host>:<port>
        // it often needs to accept the incoming connection which is a blocking
        // system call. Allowing access to the bound port using a predicate allows
        // us to wait for the port predicate to be set to a non-zero value from
        // another thread in an efficient manor.
        if (predicate)
            predicate->SetValue (port, eBroadcastAlways);
        socket = listen_socket.release();
    }

    return error;
}

Error Socket::UdpConnect(llvm::StringRef host_and_port, bool child_processes_inherit, Socket *&send_socket, Socket *&recv_socket)
{
    Log *log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_CONNECTION));
    if (log)
        log->Printf ("Socket::%s (host/port = %s)", __FUNCTION__, host_and_port.data());

    return UDPSocket::Connect(host_and_port, child_processes_inherit, send_socket, recv_socket);
}

Error Socket::UnixDomainConnect(llvm::StringRef name, bool child_processes_inherit, Socket *&socket)
{
    Error error;
    std::unique_ptr<Socket> connect_socket(Create(ProtocolUnixDomain, child_processes_inherit, error));
    if (error.Fail())
        return error;

    error = connect_socket->Connect(name);
    if (error.Success())
      socket = connect_socket.release();

    return error;
}

Error Socket::UnixDomainAccept(llvm::StringRef name, bool child_processes_inherit, Socket *&socket)
{
    Error error;
    std::unique_ptr<Socket> listen_socket(Create(ProtocolUnixDomain, child_processes_inherit, error));
    if (error.Fail())
        return error;

    error = listen_socket->Listen(name, 5);
    if (error.Fail())
        return error;

    error = listen_socket->Accept(name, child_processes_inherit, socket);
    return error;
}

Error
Socket::UnixAbstractConnect(llvm::StringRef name, bool child_processes_inherit, Socket *&socket)
{
    Error error;
    std::unique_ptr<Socket> connect_socket(Create(ProtocolUnixAbstract, child_processes_inherit, error));
    if (error.Fail())
        return error;

    error = connect_socket->Connect(name);
    if (error.Success())
      socket = connect_socket.release();
    return error;
}

Error
Socket::UnixAbstractAccept(llvm::StringRef name, bool child_processes_inherit, Socket *&socket)
{
    Error error;
    std::unique_ptr<Socket> listen_socket(Create(ProtocolUnixAbstract,child_processes_inherit, error));
    if (error.Fail())
        return error;

    error = listen_socket->Listen(name, 5);
    if (error.Fail())
        return error;

    error = listen_socket->Accept(name, child_processes_inherit, socket);
    return error;
}

bool
Socket::DecodeHostAndPort(llvm::StringRef host_and_port,
                          std::string &host_str,
                          std::string &port_str,
                          int32_t& port,
                          Error *error_ptr)
{
    static RegularExpression g_regex ("([^:]+):([0-9]+)");
    RegularExpression::Match regex_match(2);
    if (g_regex.Execute (host_and_port.data(), &regex_match))
    {
        if (regex_match.GetMatchAtIndex (host_and_port.data(), 1, host_str) &&
            regex_match.GetMatchAtIndex (host_and_port.data(), 2, port_str))
        {
            bool ok = false;
            port = StringConvert::ToUInt32 (port_str.c_str(), UINT32_MAX, 10, &ok);
            if (ok && port < UINT16_MAX)
            {
                if (error_ptr)
                    error_ptr->Clear();
                return true;
            }
            // port is too large
            if (error_ptr)
                error_ptr->SetErrorStringWithFormat("invalid host:port specification: '%s'", host_and_port.data());
            return false;
        }
    }

    // If this was unsuccessful, then check if it's simply a signed 32-bit integer, representing
    // a port with an empty host.
    host_str.clear();
    port_str.clear();
    bool ok = false;
    port = StringConvert::ToUInt32 (host_and_port.data(), UINT32_MAX, 10, &ok);
    if (ok && port < UINT16_MAX)
    {
        port_str = host_and_port;
        if (error_ptr)
            error_ptr->Clear();
        return true;
    }

    if (error_ptr)
        error_ptr->SetErrorStringWithFormat("invalid host:port specification: '%s'", host_and_port.data());
    return false;
}

IOObject::WaitableHandle Socket::GetWaitableHandle()
{
    // TODO: On Windows, use WSAEventSelect
    return m_socket;
}

Error Socket::Read (void *buf, size_t &num_bytes)
{
    Error error;
    int bytes_received = 0;
    do
    {
        bytes_received = ::recv (m_socket, static_cast<char *>(buf), num_bytes, 0);
    } while (bytes_received < 0 && IsInterrupted ());

    if (bytes_received < 0)
    {
        SetLastError (error);
        num_bytes = 0;
    }
    else
        num_bytes = bytes_received;

    Log *log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_COMMUNICATION)); 
    if (log)
    {
        log->Printf ("%p Socket::Read() (socket = %" PRIu64 ", src = %p, src_len = %" PRIu64 ", flags = 0) => %" PRIi64 " (error = %s)",
                     static_cast<void*>(this), 
                     static_cast<uint64_t>(m_socket),
                     buf,
                     static_cast<uint64_t>(num_bytes),
                     static_cast<int64_t>(bytes_received),
                     error.AsCString());
    }

    return error;
}

Error Socket::Write (const void *buf, size_t &num_bytes)
{
    Error error;
    int bytes_sent = 0;
    do
    {
        bytes_sent = Send(buf, num_bytes);
    } while (bytes_sent < 0 && IsInterrupted ());

    if (bytes_sent < 0)
    {
        SetLastError (error);
        num_bytes = 0;
    }
    else
        num_bytes = bytes_sent;

    Log *log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_COMMUNICATION));
    if (log)
    {
        log->Printf ("%p Socket::Write() (socket = %" PRIu64 ", src = %p, src_len = %" PRIu64 ", flags = 0) => %" PRIi64 " (error = %s)",
                        static_cast<void*>(this), 
                        static_cast<uint64_t>(m_socket),
                        buf,
                        static_cast<uint64_t>(num_bytes),
                        static_cast<int64_t>(bytes_sent),
                        error.AsCString());
    }

    return error;
}

Error Socket::PreDisconnect()
{
    Error error;
    return error;
}

Error Socket::Close()
{
    Error error;
    if (!IsValid() || !m_should_close_fd)
        return error;

    Log *log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_CONNECTION));
    if (log)
        log->Printf ("%p Socket::Close (fd = %i)", static_cast<void*>(this), m_socket);

#if defined(_WIN32)
    bool success = !!closesocket(m_socket);
#else
    bool success = !!::close (m_socket);
#endif
    // A reference to a FD was passed in, set it to an invalid value
    m_socket = kInvalidSocketValue;
    if (!success)
    {
        SetLastError (error);
    }

    return error;
}


int Socket::GetOption(int level, int option_name, int &option_value)
{
    get_socket_option_arg_type option_value_p = reinterpret_cast<get_socket_option_arg_type>(&option_value);
    socklen_t option_value_size = sizeof(int);
    return ::getsockopt(m_socket, level, option_name, option_value_p, &option_value_size);
}

int Socket::SetOption(int level, int option_name, int option_value)
{
    set_socket_option_arg_type option_value_p = reinterpret_cast<get_socket_option_arg_type>(&option_value);
    return ::setsockopt(m_socket, level, option_name, option_value_p, sizeof(option_value));
}

size_t Socket::Send(const void *buf, const size_t num_bytes)
{
    return ::send (m_socket, static_cast<const char *>(buf), num_bytes, 0);
}

void Socket::SetLastError(Error &error)
{
#if defined(_WIN32)
    error.SetError(::WSAGetLastError(), lldb::eErrorTypeWin32);
#else
    error.SetErrorToErrno();
#endif
}

NativeSocket
Socket::CreateSocket(const int domain,
                     const int type,
                     const int protocol,
                     bool child_processes_inherit,
                     Error& error)
{
    error.Clear();
    auto socketType = type;
#ifdef SOCK_CLOEXEC
    if (!child_processes_inherit)
        socketType |= SOCK_CLOEXEC;
#endif
    auto sock = ::socket (domain, socketType, protocol);
    if (sock == kInvalidSocketValue)
        SetLastError(error);

    return sock;
}

NativeSocket
Socket::AcceptSocket(NativeSocket sockfd,
                     struct sockaddr *addr,
                     socklen_t *addrlen,
                     bool child_processes_inherit,
                     Error& error)
{
    error.Clear();
#if defined(ANDROID_ARM_BUILD_STATIC) || defined(ANDROID_MIPS_BUILD_STATIC)
    // Temporary workaround for statically linking Android lldb-server with the
    // latest API.
    int fd = syscall(__NR_accept, sockfd, addr, addrlen);
    if (fd >= 0 && !child_processes_inherit)
    {
        int flags = ::fcntl(fd, F_GETFD);
        if (flags != -1 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != -1)
            return fd;
        SetLastError(error);
        close(fd);
    }
    return fd;
#elif defined(SOCK_CLOEXEC)
    int flags = 0;
    if (!child_processes_inherit) {
        flags |= SOCK_CLOEXEC;
    }
#if defined(__NetBSD__)
    NativeSocket fd = ::paccept (sockfd, addr, addrlen, nullptr, flags);
#else
    NativeSocket fd = ::accept4 (sockfd, addr, addrlen, flags);
#endif
#else
    NativeSocket fd = ::accept (sockfd, addr, addrlen);
#endif
    if (fd == kInvalidSocketValue)
        SetLastError(error);
    return fd;
}
