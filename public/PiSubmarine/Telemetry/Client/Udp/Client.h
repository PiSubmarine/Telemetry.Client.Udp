#pragma once

#include <chrono>
#include <optional>

#include "PiSubmarine/Error/Api/Error.h"
#include "PiSubmarine/Lease/Api/ILeaseIssuer.h"
#include "PiSubmarine/Telemetry/Api/ISource.h"
#include "PiSubmarine/Telemetry/IDeserializer.h"
#include "PiSubmarine/Time/ITickable.h"
#include "PiSubmarine/Udp/Api/IReceiver.h"
#include "PiSubmarine/Udp/Api/ISender.h"

namespace PiSubmarine::Telemetry::Client::Udp
{
    class Client final : public Api::ISource, public Time::ITickable
    {
    public:
        Client(
            Lease::Api::ILeaseIssuer& leaseIssuer,
            const ::PiSubmarine::Telemetry::IDeserializer& deserializer,
            ::PiSubmarine::Udp::Api::IReceiver& receiver,
            ::PiSubmarine::Udp::Api::ISender& sender,
            ::PiSubmarine::Udp::Api::Endpoint serverEndpoint,
            std::chrono::milliseconds acquireRetryInterval = std::chrono::seconds(1),
            std::chrono::milliseconds subscribeRetryInterval = std::chrono::seconds(1));

        ~Client() override;

        [[nodiscard]] Error::Api::Result<Api::Snapshot> GetSnapshot() const override;
        void Tick(const std::chrono::nanoseconds& uptime, const std::chrono::nanoseconds& deltaTime) override;

    private:
        [[nodiscard]] static Lease::Api::ResourceId MakeTelemetryResourceId();
        [[nodiscard]] static std::chrono::nanoseconds ComputeRenewInterval(const Lease::Api::Lease& lease);
        [[nodiscard]] bool EnsureLease(const std::chrono::nanoseconds& uptime);
        void RenewLease(const std::chrono::nanoseconds& uptime);
        void TrySubscribe(const std::chrono::nanoseconds& uptime);
        void ReceiveDatagrams();

        Lease::Api::ILeaseIssuer& m_LeaseIssuer;
        const ::PiSubmarine::Telemetry::IDeserializer& m_Deserializer;
        ::PiSubmarine::Udp::Api::IReceiver& m_Receiver;
        ::PiSubmarine::Udp::Api::ISender& m_Sender;
        ::PiSubmarine::Udp::Api::Endpoint m_ServerEndpoint;
        std::chrono::nanoseconds m_AcquireRetryInterval;
        std::chrono::nanoseconds m_SubscribeRetryInterval;

        std::optional<Api::Snapshot> m_Snapshot;
        std::optional<Error::Api::Error> m_LastError;
        std::optional<Lease::Api::Lease> m_Lease;
        std::chrono::nanoseconds m_NextAcquireAttempt{0};
        std::chrono::nanoseconds m_NextRenewTime{0};
        std::chrono::nanoseconds m_NextSubscribeAttempt{0};
        bool m_IsSubscriptionPending = false;
    };
}
