#include "PiSubmarine/Telemetry/Client/Udp/Source.h"

namespace PiSubmarine::Telemetry::Client::Udp
{
    Source::Source(Api::IRawCache& rawCache, Api::ChannelId channel)
        : m_RawCache(rawCache)
        , m_Channel(std::move(channel))
    {
    }

    Error::Api::Result<std::vector<std::byte>> Source::GetRaw() const
    {
        return m_RawCache.GetRaw(m_Channel);
    }
}
