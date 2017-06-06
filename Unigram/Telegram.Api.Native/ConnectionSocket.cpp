#include "pch.h"
#include <Ws2tcpip.h>
#include "ConnectionSocket.h"
#include "ConnectionManager.h"
#include "Datacenter.h"
#include "Helpers\COMHelper.h"

#define BreakIfSocketError(result, method) \
	if((result = method) == SOCKET_ERROR) \
		break

#define BreakIfError(result, method) \
	if((result = method) != NO_ERROR) \
		break

#define SOCKET_SEND_BUFFER_SIZE 0
#define SOCKET_RECEIVE_BUFFER_SIZE 1024 * 128
#define SOCKET_CLOSE_NONE 0
#define SOCKET_CLOSE_RAISEEVENT 1
#define SOCKET_CLOSE_JOINTHREAD 2
#define SOCKET_CLOSE_DEFAULT (SOCKET_CLOSE_RAISEEVENT | SOCKET_CLOSE_JOINTHREAD)

using namespace Telegram::Api::Native;


ConnectionSocket::ConnectionSocket() :
	m_socket(INVALID_SOCKET),
	m_timeout({})
{
}

ConnectionSocket::~ConnectionSocket()
{
}

HRESULT ConnectionSocket::Close()
{
	return CloseSocket(NO_ERROR, SOCKET_CLOSE_JOINTHREAD);
}

HRESULT ConnectionSocket::ConnectSocket(ConnectionManager* connectionManager, ServerEndpoint const* endpoint, boolean ipv6, UINT32 timeoutMs)
{
	if (m_socket != INVALID_SOCKET)
	{
		return E_NOT_VALID_STATE;
	}

	ULARGE_INTEGER timeout;
	timeout.QuadPart = timeoutMs * -10000LL;
	m_timeout.dwHighDateTime = timeout.HighPart;
	m_timeout.dwLowDateTime = timeout.LowPart;

	sockaddr_storage socketAddress = {};
	if (ipv6)
	{
		auto socketAddressIpv6 = reinterpret_cast<sockaddr_in6*>(&socketAddress);
		socketAddressIpv6->sin6_family = AF_INET6;
		socketAddressIpv6->sin6_port = htons(endpoint->Port);

		if (InetPton(AF_INET6, endpoint->Address.c_str(), &socketAddressIpv6->sin6_addr) != 1)
		{
			return WSAGetLastHRESULT();
		}
	}
	else
	{
		auto socketAddressIpv4 = reinterpret_cast<sockaddr_in*>(&socketAddress);
		socketAddressIpv4->sin_family = AF_INET;
		socketAddressIpv4->sin_port = htons(endpoint->Port);

		if (InetPton(AF_INET, endpoint->Address.c_str(), &socketAddressIpv4->sin_addr) != 1)
		{
			return WSAGetLastHRESULT();
		}
	}

	m_sendBuffer.resize(SOCKET_SEND_BUFFER_SIZE);
	m_receiveBuffer = std::make_unique<BYTE[]>(SOCKET_RECEIVE_BUFFER_SIZE);

	m_socketConnectedEvent.Attach(CreateEvent(nullptr, TRUE, FALSE, nullptr));
	if (!m_socketConnectedEvent.IsValid())
	{
		return GetLastHRESULT();
	}

	m_socketEvent.Attach(WSACreateEvent());
	if (!m_socketEvent.IsValid())
	{
		return WSAGetLastHRESULT();
	}

	if ((m_socket = socket(socketAddress.ss_family, SOCK_STREAM, 0)) == INVALID_SOCKET)
	{
		return GetLastErrorAndCloseSocket(SOCKET_CLOSE_NONE);
	}

	int noDelay = 1;
	setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&noDelay), sizeof(int));

	HRESULT result;
	if (FAILED(result = AttachToThreadpool(connectionManager)))
	{
		CloseSocket(WIN32_FROM_HRESULT(result), SOCKET_CLOSE_NONE);

		return result;
	}

	SetThreadpoolWait(EventObjectT::GetHandle(), GetSocketEvent(), &m_timeout);

	if (WSAEventSelect(m_socket, m_socketEvent.Get(), FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE) == SOCKET_ERROR)
	{
		return GetLastErrorAndCloseSocket(SOCKET_CLOSE_JOINTHREAD);
	}

	if (connect(m_socket, reinterpret_cast<sockaddr*>(&socketAddress), ipv6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in)) == SOCKET_ERROR)
	{
		auto wsaLastError = WSAGetLastError();
		if (wsaLastError != WSAEWOULDBLOCK)
		{
			CloseSocket(wsaLastError, SOCKET_CLOSE_JOINTHREAD);

			return HRESULT_FROM_WIN32(wsaLastError);
		}
	}

	return S_OK;
}

HRESULT ConnectionSocket::DisconnectSocket(boolean immediate)
{
	if (immediate)
	{
		return CloseSocket(NO_ERROR, SOCKET_CLOSE_RAISEEVENT);
	}

	if (m_socket == INVALID_SOCKET)
	{
		return E_NOT_VALID_STATE;
	}

	if (shutdown(m_socket, SD_BOTH) == SOCKET_ERROR)
	{
		return WSAGetLastHRESULT();
	}

	return S_OK;
}

HRESULT ConnectionSocket::SendData(BYTE const* buffer, UINT32 length)
{
	/*if (buffer == nullptr)
	{
		return E_POINTER;
	}*/

	if (m_socket == INVALID_SOCKET)
	{
		return E_NOT_VALID_STATE;
	}

	WaitForSingleObject(m_socketConnectedEvent.Get(), INFINITE);

	int bytesSent = send(m_socket, reinterpret_cast<const char*>(buffer), length, 0);
	if (bytesSent == SOCKET_ERROR)
	{
		auto wsaLastError = WSAGetLastError();
		if (wsaLastError != WSAEWOULDBLOCK)
		{
			return HRESULT_FROM_WIN32(wsaLastError);
		}
	}
	else if (static_cast<UINT32>(bytesSent) < length)
	{
		auto remainingSize = length - bytesSent;
		auto availableSize = m_sendBuffer.size();

		m_sendBuffer.resize(availableSize + remainingSize);
		CopyMemory(m_sendBuffer.data() + availableSize, buffer, remainingSize);
	}

	return S_OK;
}

HRESULT ConnectionSocket::GetLastErrorAndCloseSocket(BYTE flags)
{
	auto wsaLastError = WSAGetLastError();

	CloseSocket(wsaLastError, flags);

	return HRESULT_FROM_WIN32(wsaLastError);
}

HRESULT ConnectionSocket::CloseSocket(int wsaError, BYTE flags)
{
	if (m_socket == INVALID_SOCKET)
	{
		return E_NOT_VALID_STATE;
	}

	if (closesocket(m_socket) == SOCKET_ERROR)
	{
		return WSAGetLastHRESULT();
	}

	m_socket = INVALID_SOCKET;

	DetachFromThreadpool(flags & SOCKET_CLOSE_JOINTHREAD);

	m_socketEvent.Close();
	m_socketConnectedEvent.Close();
	m_sendBuffer = {};
	m_receiveBuffer.reset();

	if (flags & SOCKET_CLOSE_RAISEEVENT)
	{
		return OnSocketDisconnected(wsaError);
	}

	return S_OK;
}

HRESULT ConnectionSocket::OnEvent(PTP_CALLBACK_INSTANCE callbackInstance, ULONG_PTR waitResult)
{
	auto lock = LockCriticalSection();

	if (waitResult != WAIT_OBJECT_0)
	{
		return CloseSocket(ERROR_TIMEOUT, SOCKET_CLOSE_RAISEEVENT);
	}

	if (m_socket == INVALID_SOCKET)
	{
		return S_FALSE;
	}

	WSANETWORKEVENTS networkEvents;
	if (WSAEnumNetworkEvents(m_socket, m_socketEvent.Get(), &networkEvents) == SOCKET_ERROR)
	{
		return GetLastErrorAndCloseSocket(SOCKET_CLOSE_RAISEEVENT);
	}

	if (networkEvents.lNetworkEvents & FD_CLOSE)
	{
		return CloseSocket(networkEvents.iErrorCode[FD_CLOSE_BIT], SOCKET_CLOSE_RAISEEVENT);
	}

	int wsaLastError;

	do
	{
		if (networkEvents.lNetworkEvents & FD_CONNECT)
		{
			BreakIfError(wsaLastError, networkEvents.iErrorCode[FD_CONNECT_BIT]);

			SetEvent(m_socketConnectedEvent.Get());

			HRESULT result;
			if (FAILED(result = OnSocketConnected()))
			{
				CloseSocket(WIN32_FROM_HRESULT(result), SOCKET_CLOSE_RAISEEVENT);

				return result;
			}
		}

		if (networkEvents.lNetworkEvents & FD_WRITE)
		{
			BreakIfError(wsaLastError, networkEvents.iErrorCode[FD_WRITE_BIT]);

			auto availableBytes = m_sendBuffer.size();
			if (availableBytes > 0)
			{
				int sentBytes;
				if ((sentBytes = send(m_socket, reinterpret_cast<char*>(m_sendBuffer.data()), static_cast<int>(availableBytes), 0)) == SOCKET_ERROR)
				{
					if ((wsaLastError = WSAGetLastError()) != WSAEWOULDBLOCK)
					{
						break;
					}
				}

				auto remainingBytes = availableBytes - sentBytes;
				MoveMemory(m_sendBuffer.data(), m_sendBuffer.data() + sentBytes, remainingBytes);

				m_sendBuffer.resize(remainingBytes);
			}
		}

		if (networkEvents.lNetworkEvents & FD_READ)
		{
			BreakIfError(wsaLastError, networkEvents.iErrorCode[FD_READ_BIT]);

			HRESULT result;
			int receivedBytes;
			while ((receivedBytes = recv(m_socket, reinterpret_cast<char*>(m_receiveBuffer.get()), SOCKET_RECEIVE_BUFFER_SIZE, 0)) > 0)
			{
				if (FAILED(result = OnDataReceived(m_receiveBuffer.get(), receivedBytes)))
				{
					CloseSocket(WIN32_FROM_HRESULT(result), SOCKET_CLOSE_RAISEEVENT);

					return result;
				}
			}

			if (receivedBytes == SOCKET_ERROR && (wsaLastError = WSAGetLastError()) != WSAEWOULDBLOCK)
			{
				break;
			}
		}

		SetThreadpoolWait(EventObjectT::GetHandle(), m_socketEvent.Get(), &m_timeout);
		return S_OK;
	} while (false);

	CloseSocket(wsaLastError, SOCKET_CLOSE_RAISEEVENT);

	return HRESULT_FROM_WIN32(wsaLastError);
}