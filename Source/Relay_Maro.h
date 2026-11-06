#pragma once

#include <memory>
#include <string>

namespace maro {

class RelayTunnel {
public:
    static std::shared_ptr<RelayTunnel> host(std::string endpoint, unsigned short localPort);
    static std::shared_ptr<RelayTunnel> join(std::string endpoint, std::string roomCode);

    ~RelayTunnel();
    bool ready() const;
    std::string roomCode() const;
    std::string error() const;
    unsigned short localPort() const;

private:
    struct Impl;
    explicit RelayTunnel(bool hosting, std::string endpoint, std::string code, unsigned short port);
    std::unique_ptr<Impl> impl_;
};

}
