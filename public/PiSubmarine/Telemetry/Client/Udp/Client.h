#pragma once

#include <cstddef>
#include <chrono>
#include <map>
#include <optional>
#include <vector>

#include "PiSubmarine/Error/Api/Error.h"
#include "PiSubmarine/Lease/Api/ILeaseIssuer.h"
#include "PiSubmarine/Security/Aead/Api/IProvider.h"
#include "PiSubmarine/Security/Api/INonceProvider.h"
#include "PiSubmarine/Telemetry/Api/ChannelId.h"
#include "PiSubmarine/Telemetry/Api/IRawCache.h"
#include "PiSubmarine/Time/ITickable.h"
#include "PiSubmarine/Udp/Api/IReceiver.h"
#include "PiSubmarine/Udp/Api/ISender.h"

namespace PiSubmarine::Telemetry::Client::Udp
{
    class Client final : public Api::IRawCache, public Time::ITickable
    {
    public:
        Client(
            Lease::Api::ILeaseIssuer& leaseIssuer,
            const ::PiSubmarine::Security::Aead::Api::IProvider& aeadProvider,
            ::PiSubmarine::Security::Api::INonceProvider& nonceProvider,
            ::PiSubmarine::Udp::Api::IReceiver& receiver,
            ::PiSubmarine::Udp::Api::ISender& sender,
            ::PiSubmarine::Udp::Api::Endpoint serverEndpoint,
            std::chrono::milliseconds acquireRetryInterval = std::chrono::seconds(1),
            std::chrono::milliseconds subscribeRetryInterval = std::chrono::seconds(1));

        ~Client() override;

        [[nodiscard]] Error::Api::Result<std::vector<std::byte>> GetRaw(const Api::ChannelId& channel) const override;
        [[nodiscard]] bool HasLease() const noexcept;
        void Tick(const std::chrono::nanoseconds& uptime, const std::chrono::nanoseconds& deltaTime) override;

    private:
        [[nodiscard]] static Lease::Api::ResourceId MakeTelemetryResourceId();
        [[nodiscard]] static std::chrono::nanoseconds ComputeRenewInterval(const Lease::Api::Lease& lease);
        [[nodiscard]] bool EnsureLease(const std::chrono::nanoseconds& uptime);
        void RenewLease(const std::chrono::nanoseconds& uptime);
        void TrySubscribe(const std::chrono::nanoseconds& uptime);
        void ReceiveDatagrams();

        Lease::Api::ILeaseIssuer& m_LeaseIssuer;
        const ::PiSubmarine::Security::Aead::Api::IProvider& m_AeadProvider;
        ::PiSubmarine::Security::Api::INonceProvider& m_NonceProvider;
        ::PiSubmarine::Udp::Api::IReceiver& m_Receiver;
        ::PiSubmarine::Udp::Api::ISender& m_Sender;
        ::PiSubmarine::Udp::Api::Endpoint m_ServerEndpoint;
        std::chrono::nanoseconds m_AcquireRetryInterval;
        std::chrono::nanoseconds m_SubscribeRetryInterval;

        std::map<Api::ChannelId, std::vector<std::byte>> m_Payloads;
        std::optional<Error::Api::Error> m_LastError;
        std::optional<Lease::Api::Lease> m_Lease;
        std::optional<Lease::Api::LeaseSecret> m_LeaseSecret;
        std::chrono::nanoseconds m_NextAcquireAttempt{0};
        std::chrono::nanoseconds m_NextRenewTime{0};
        std::chrono::nanoseconds m_NextSubscribeAttempt{0};
        bool m_IsSubscriptionPending = false;
        bool m_HasCurrentDatagram = false;
    };
}
