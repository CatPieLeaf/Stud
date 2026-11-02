// gfxstream's guest IOStream over a Unix socket to Stud's host.
//
// The first byte a connection sends says what it is: 'R' for a render
// channel (the GLES and renderControl encoders' stream, one per GL thread)
// or 'P' for the process handshake, which answers with this process's
// 64-bit id, the job an emulator's GLProcessPipe service does.
#pragma once

#include <cstddef>
#include <cstdint>

#include "gfxstream/guest/IOStream.h"

class StudStream : public gfxstream::guest::IOStream {
public:
    explicit StudStream(size_t bufsize);
    ~StudStream() override;

    // Connects and announces a render channel.
    int connect(const char* serviceName = nullptr) override;
    // A connection of its own: announces the handshake, reads the id.
    uint64_t processPipeInit() override;

    void* allocBuffer(size_t minSize) override;
    int commitBuffer(size_t size) override;
    const unsigned char* readFully(void* buf, size_t len) override;
    const unsigned char* commitBufferAndReadFully(size_t size, void* buf, size_t len) override;
    const unsigned char* read(void* buf, size_t* inout_len) override;
    int writeFully(const void* buf, size_t len) override;

    // A connection to the socket the host listens on (STUD_GFXSTREAM_SOCKET),
    // announced as `tag`; -1 if there is none.
    static int open_tagged(char tag);

private:

    int m_fd = -1;
    unsigned char* m_buf = nullptr;
    size_t m_bufsize = 0;
};
