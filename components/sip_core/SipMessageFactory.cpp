#include "SipMessageFactory.hpp"
#include <memory>

// NOTE (PoC fork): pocket-dial's original factory pulled messages from
// RequestsHandler's static pool (server-side, pre-allocated). For the client
// spike we don't run the handler, so we construct directly. Behaviour is
// otherwise identical: an SDP body yields a SipSdpMessage, else a SipMessage.
std::optional<std::shared_ptr<SipMessage>> SipMessageFactory::createMessage(std::string message, sockaddr_in src)
{
	if (message.empty())
	{
		return std::nullopt;
	}

	std::shared_ptr<SipMessage> msg;
	if (containsSdp(message))
	{
		msg = std::make_shared<SipSdpMessage>(std::move(message), src);
	}
	else
	{
		msg = std::make_shared<SipMessage>(std::move(message), src);
	}
	return msg;
}

bool SipMessageFactory::containsSdp(const std::string& message) const
{
	return message.find(SDP_CONTENT_TYPE) != std::string::npos;
}
