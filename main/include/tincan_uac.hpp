#ifndef TINCAN_UAC_HPP
#define TINCAN_UAC_HPP

#include <string>
#include <atomic>

class TincanUac
{
public:
    TincanUac();
    ~TincanUac();

    bool init(const std::string& localIp, int localSipPort, int localRtpPort,
              const std::string& serverIp, int serverPort,
              const std::string& selfExt, const std::string& calleeExt);

    void startMediaLoop();
    void stopMediaLoop();
    void setPttState(bool active);
    bool isPttActive() const { return _pttActive.load(); }

private:
    std::atomic<bool> _running{false};
    std::atomic<bool> _pttActive{false};
    std::string _selfExt;
    std::string _calleeExt;
};

#endif // TINCAN_UAC_HPP
