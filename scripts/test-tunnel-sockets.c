// Regression harness: builds the production PlatformSockets.c and ENet win32.c.
#include "Limelight-internal.h"
#include <stdio.h>
#include <stdarg.h>

CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
uint16_t RtspPortNumber = 48010;

static void logMessage(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s (WSA %d)\n", __LINE__, #x, WSAGetLastError()); return 1; } } while (0)

static struct sockaddr_storage destination;
static SOCKET sharedSocket;

static DWORD WINAPI parallelSend(void* arg) {
    (void)arg;
    return sendUdpSocket(sharedSocket, "parallel", 8, (struct sockaddr*)&destination,
                         sizeof(struct sockaddr_in)) == 8 ? 0 : 1;
}

static SOCKET udp(int family) {
    SOCKET s = bindUdpSocket(family, NULL, 0, 0, SOCK_QOS_TYPE_BEST_EFFORT);
    if (s != INVALID_SOCKET) setNonFatalRecvTimeoutMs(s, 2000);
    return s;
}

int main(int argc, char** argv) {
    if (argc != 2 && argc != 5) return 2;
    ListenerCallbacks.logMessage = logMessage;
    CHECK(initializePlatformSockets() == 0);
    CHECK(enet_initialize() == 0);
    if (argc == 5) {
        // Real end-to-end checks through Xray, not a mock implementation.
        LiSetSocks5Proxy(true, "127.0.0.1", (uint16_t)atoi(argv[1]), argv[2], argv[3]);
        struct sockaddr_in* remote = (struct sockaddr_in*)&destination;
        remote->sin_family = AF_INET;
        inet_pton(AF_INET, "1.1.1.1", &remote->sin_addr);
        SOCKET tcp = connectTcpSocket(&destination, sizeof(*remote), 80, 10);
        CHECK(tcp != INVALID_SOCKET);
        char request[] = "GET / HTTP/1.1\r\nHost: 1.1.1.1\r\nConnection: close\r\n\r\n";
        CHECK(sendMtuSafe(tcp, request, sizeof(request) - 1) == sizeof(request) - 1);
        char buffer[4096];
        struct pollfd ready = {tcp, POLLIN, 0};
        CHECK(pollSockets(&ready, 1, 10000) > 0);
        CHECK(recv(tcp, buffer, sizeof(buffer), 0) > 5 && memcmp(buffer, "HTTP/", 5) == 0);
        closeSocket(tcp);
        puts("PASS: production TCP wrapper through actual VLESS/Reality server");
        const unsigned char query[] = {0x4d,0x4c,1,0,0,1,0,0,0,0,0,0,7,'e','x','a','m','p','l','e',3,'c','o','m',0,0,1,0,1};
        remote->sin_port = htons(53);
        SOCKET s = udp(AF_INET);
        setNonFatalRecvTimeoutMs(s, 10000);
        CHECK(sendUdpSocket(s, (char*)query, sizeof(query), (struct sockaddr*)remote, sizeof(*remote)) == sizeof(query));
        CHECK(recvUdpSocket(s, buffer, sizeof(buffer), false) >= 12);
        CHECK(buffer[0] == 0x4d && buffer[1] == 0x4c && ((unsigned char)buffer[2] & 0x80));
        closeSocket(s);
        puts("PASS: production UDP wrapper through actual VLESS/Reality server");
        ENetAddress peer = {0}, local = {0}, returned = {0};
        memcpy(&peer.address, remote, sizeof(*remote)); peer.addressLength = sizeof(*remote);
        ((struct sockaddr_in*)&local.address)->sin_family = AF_INET; local.addressLength = sizeof(*remote);
        ENetSocket enet = enet_socket_create(AF_INET, ENET_SOCKET_TYPE_DATAGRAM);
        CHECK(enet_socket_bind(enet, &local) == 0);
        setNonFatalRecvTimeoutMs(enet, 10000);
        ENetBuffer parts[2] = {{12, (void*)query}, {sizeof(query)-12, (void*)(query+12)}};
        CHECK(enet_socket_send(enet, &peer, &local, parts, 2) == sizeof(query));
        ENetBuffer reply = {sizeof(buffer), buffer};
        CHECK(enet_socket_receive(enet, &returned, &local, &reply, 1) >= 12);
        CHECK(buffer[0] == 0x4d && buffer[1] == 0x4c && ((unsigned char)buffer[2] & 0x80));
        enet_socket_destroy(enet);
        puts("PASS: production ENet scatter/gather through actual VLESS/Reality server");
        enet_deinitialize(); cleanupPlatformSockets();
        return 0;
    }
    LiSetSocks5Proxy(true, "127.0.0.1", (uint16_t)atoi(argv[1]), "testuser", "testpass");
    struct sockaddr_in* dest = (struct sockaddr_in*)&destination;
    dest->sin_family = AF_INET;
    dest->sin_port = htons(47998);
    inet_pton(AF_INET, "203.0.113.10", &dest->sin_addr);

    // TCP CONNECT and RTSP response; direct routing cannot reach this TEST-NET host.
    SOCKET tcp = connectTcpSocket(&destination, sizeof(*dest), 48010, 2);
    CHECK(tcp != INVALID_SOCKET);
    char request[] = "OPTIONS rtsp://203.0.113.10 RTSP/1.0\r\nCSeq: 1\r\n\r\n";
    CHECK(sendMtuSafe(tcp, request, sizeof(request) - 1) == sizeof(request) - 1);
    char response[128] = {0};
    int n = recv(tcp, response, sizeof(response), 0);
    CHECK(n >= 12 && memcmp(response, "RTSP/1.0 200", 12) == 0);
    closeSocket(tcp);
    tcp = connectTcpSocket(&destination, sizeof(*dest), 1, 2);
    CHECK(tcp == INVALID_SOCKET);
    puts("PASS: TCP CONNECT, fragmented negotiation, RTSP, rejected CONNECT, no recursion");

    // Repeated close/reconnect exceeds table capacity to catch leaked slots.
    for (int i = 0; i < 70; i++) {
        SOCKET s = udp(AF_INET);
        CHECK(s != INVALID_SOCKET);
        CHECK(sendUdpSocket(s, "echo", 4, (struct sockaddr*)dest, sizeof(*dest)) == 4);
        struct sockaddr_storage source = {0};
        SOCKADDR_LEN sourceLen = sizeof(source);
        char buffer[32];
        CHECK(recvUdpSocketFrom(s, buffer, sizeof(buffer), (struct sockaddr*)&source, &sourceLen, false) == 4);
        CHECK(memcmp(buffer, "echo", 4) == 0);
        CHECK(source.ss_family == AF_INET);
        CHECK(((struct sockaddr_in*)&source)->sin_addr.s_addr == dest->sin_addr.s_addr);
        CHECK(((struct sockaddr_in*)&source)->sin_port == dest->sin_port);
        closeSocket(s);
    }
    puts("PASS: 70 UDP reconnects, payload decoding, peer address, slot cleanup");

    sharedSocket = udp(AF_INET);
    CHECK(sharedSocket != INVALID_SOCKET);
    HANDLE threads[8];
    for (int i = 0; i < 8; i++) threads[i] = CreateThread(NULL, 0, parallelSend, NULL, 0, NULL);
    CHECK(WaitForMultipleObjects(8, threads, TRUE, 15000) == WAIT_OBJECT_0);
    for (int i = 0; i < 8; i++) {
        DWORD code;
        CHECK(GetExitCodeThread(threads[i], &code) && code == 0);
        CloseHandle(threads[i]);
        char buffer[32];
        CHECK(recvUdpSocket(sharedSocket, buffer, sizeof(buffer), false) == 8);
        CHECK(memcmp(buffer, "parallel", 8) == 0);
    }
    CHECK(setSocketNonBlocking(sharedSocket, true) == 0);
    char buffer[64];
    CHECK(recvUdpSocket(sharedSocket, buffer, sizeof(buffer), false) == 0);
    closeSocket(sharedSocket);
    puts("PASS: concurrent association initialization and nonblocking empty receive");

    SOCKET s = udp(AF_INET);
    CHECK(sendUdpSocket(s, "malformed", 9, (struct sockaddr*)dest, sizeof(*dest)) == 9);
    CHECK(recvUdpSocket(s, buffer, sizeof(buffer), false) == 9);
    CHECK(memcmp(buffer, "malformed", 9) == 0);
    CHECK(sendUdpSocket(s, "oversize", 8, (struct sockaddr*)dest, sizeof(*dest)) == 8);
    CHECK(recvUdpSocket(s, buffer, sizeof(buffer), false) < 0);
    CHECK(WSAGetLastError() == WSAEMSGSIZE);
    closeSocket(s);
    puts("PASS: malformed/fragmented/spoofed UDP discarded, oversize detected");

    s = udp(AF_INET6);
    CHECK(s != INVALID_SOCKET);
    CHECK(sendUdpSocket(s, "ipv6socket", 10, (struct sockaddr*)dest, sizeof(*dest)) == 10);
    CHECK(recvUdpSocket(s, buffer, sizeof(buffer), false) == 10);
    closeSocket(s);
    puts("PASS: dual-stack application socket with IPv4 loopback relay");

    ENetAddress local = {0}, peer = {0}, returned = {0};
    memcpy(&peer.address, dest, sizeof(*dest));
    peer.addressLength = sizeof(*dest);
    ((struct sockaddr_in*)&local.address)->sin_family = AF_INET;
    local.addressLength = sizeof(*dest);
    ENetSocket enet = enet_socket_create(AF_INET, ENET_SOCKET_TYPE_DATAGRAM);
    CHECK(enet != INVALID_SOCKET);
    CHECK(enet_socket_bind(enet, &local) == 0);
    setNonFatalRecvTimeoutMs(enet, 2000);
    ENetBuffer fragments[3] = {{5, "hello"}, {1, "-"}, {5, "world"}};
    CHECK(enet_socket_send(enet, &peer, &local, fragments, 3) == 11);
    char left[5], right[6];
    ENetBuffer output[2] = {{5, left}, {6, right}};
    CHECK(enet_socket_receive(enet, &returned, &local, output, 2) == 11);
    CHECK(memcmp(left, "hello", 5) == 0 && memcmp(right, "-world", 6) == 0);
    CHECK(returned.addressLength == sizeof(*dest));
    enet_socket_destroy(enet);
    puts("PASS: ENet scatter/gather with localAddress, tunneled control packets");

    LiSetSocks5Proxy(true, "127.0.0.1", (uint16_t)atoi(argv[1]), "wrong", "wrong");
    CHECK(connectTcpSocket(&destination, sizeof(*dest), 48010, 2) == INVALID_SOCKET);
    s = udp(AF_INET);
    CHECK(sendUdpSocket(s, "echo", 4, (struct sockaddr*)dest, sizeof(*dest)) < 0);
    closeSocket(s);
    puts("PASS: bad authentication fails without direct fallback");
    LiSetSocks5Proxy(false, NULL, 0, NULL, NULL);
    enet_deinitialize();
    cleanupPlatformSockets();
    return 0;
}
