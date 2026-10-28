#pragma once

#include <FS.h>
#include <WebServer.h>

// The framework's send() logs "content length is zero" whenever the body
// follows separately, which is how every streamed response here starts (the
// runs list, run files, the ZIP), and how streamFile() ends too: a dashboard
// left open put several a second into /api/debug/log and pushed everything
// worth reading out of its 3KB. beginResponse() sends the same status line and
// headers without the warning, and streamWebFile() is streamFile() on top of it.
class DialedWebServer : public WebServer {
public:
    using WebServer::WebServer;

    void beginResponse(int code, const char* contentType) {
        String header;
        _prepareHeader(header, code, contentType, 0);
        _currentClientWrite(header.c_str(), header.length());
    }

    // As streamFile(): a name ending in .gz is sent with "Content-Encoding: gzip".
    size_t streamWebFile(File& file, const char* contentType) {
        setContentLength(file.size());
        if (String(file.name()).endsWith(".gz"))
            sendHeader("Content-Encoding", "gzip");

        beginResponse(200, contentType);
        return _currentClient.write(file);
    }
};
