#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "PiSubmarine/Telemetry/Api/IRawCache.h"
#include "PiSubmarine/Telemetry/Api/IRawSource.h"

namespace PiSubmarine::Telemetry::Client::Udp
{
    class Source final : public Api::IRawSource
    {
    public:
        Source(Api::IRawCache& rawCache, Api::ChannelId channel);

        [[nodiscard]] Error::Api::Result<std::vector<std::byte>> GetRaw() const override;

    private:
        Api::IRawCache& m_RawCache;
        Api::ChannelId m_Channel;
    };
}
