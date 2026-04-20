#include "PiSubmarine/Telemetry/Client/Udp/Client.h"

#include <algorithm>
#include <utility>

namespace PiSubmarine::Telemetry::Client::Udp
{
    Client::Client(
        Lease::Api::ILeaseIssuer& leaseIssuer,
        const ::PiSubmarine::Telemetry::IDeserializer& deserializer,
        ::PiSubmarine::Udp::Api::IReceiver& receiver,
        ::PiSubmarine::Udp::Api::ISender& sender,
        ::PiSubmarine::Udp::Api::Endpoint serverEndpoint,
        const std::chrono::milliseconds acquireRetryInterval,
        const std::chrono::milliseconds subscribeRetryInterval)
        : m_LeaseIssuer(leaseIssuer)
        , m_Deserializer(deserializer)
        , m_Receiver(receiver)
        , m_Sender(sender)
        , m_ServerEndpoint(std::move(serverEndpoint))
        , m_AcquireRetryInterval(std::chrono::duration_cast<std::chrono::nanoseconds>(acquireRetryInterval))
        , m_SubscribeRetryInterval(std::chrono::duration_cast<std::chrono::nanoseconds>(subscribeRetryInterval))
    {
    }

    Client::~Client()
    {
        if (!m_Lease.has_value())
        {
            return;
        }

        [[maybe_unused]] const auto releaseResult = m_LeaseIssuer.ReleaseLease(m_Lease->Id);
    }

    Api::Snapshot Client::GetSnapshot() const
    {
        return m_Snapshot;
    }

    void Client::Tick(const std::chrono::nanoseconds& uptime, const std::chrono::nanoseconds& deltaTime)
    {
        static_cast<void>(deltaTime);

        if (EnsureLease(uptime))
        {
            RenewLease(uptime);
            TrySubscribe(uptime);
        }

        ReceiveDatagrams();
    }

    Lease::Api::ResourceId Client::MakeTelemetryResourceId()
    {
        return Lease::Api::ResourceId{.Value = "telemetry-main"};
    }

    std::chrono::nanoseconds Client::ComputeRenewInterval(const Lease::Api::Lease& lease)
    {
        const auto halfDuration = lease.Duration / 2;
        const auto boundedDuration = std::max(halfDuration, std::chrono::milliseconds(1));
        return std::chrono::duration_cast<std::chrono::nanoseconds>(boundedDuration);
    }

    bool Client::EnsureLease(const std::chrono::nanoseconds& uptime)
    {
        if (m_Lease.has_value())
        {
            return true;
        }

        if (uptime < m_NextAcquireAttempt)
        {
            return false;
        }

        const auto acquireResult = m_LeaseIssuer.AcquireLease(Lease::Api::LeaseRequest{
            .Resource = MakeTelemetryResourceId()});
        if (!acquireResult.has_value())
        {
            m_NextAcquireAttempt = uptime + m_AcquireRetryInterval;
            return false;
        }

        m_Lease = *acquireResult;
        m_NextRenewTime = uptime + ComputeRenewInterval(*m_Lease);
        m_NextSubscribeAttempt = uptime;
        m_IsSubscriptionPending = true;
        return true;
    }

    void Client::RenewLease(const std::chrono::nanoseconds& uptime)
    {
        if (!m_Lease.has_value() || uptime < m_NextRenewTime)
        {
            return;
        }

        const auto renewResult = m_LeaseIssuer.RenewLease(m_Lease->Id);
        if (!renewResult.has_value())
        {
            m_Lease.reset();
            m_NextAcquireAttempt = uptime + m_AcquireRetryInterval;
            m_IsSubscriptionPending = false;
            return;
        }

        m_Lease = *renewResult;
        m_NextRenewTime = uptime + ComputeRenewInterval(*m_Lease);
        m_NextSubscribeAttempt = uptime;
        m_IsSubscriptionPending = true;
    }

    void Client::TrySubscribe(const std::chrono::nanoseconds& uptime)
    {
        if (!m_Lease.has_value() || !m_IsSubscriptionPending || uptime < m_NextSubscribeAttempt)
        {
            return;
        }

        std::vector<std::byte> payload;
        payload.reserve(m_Lease->Id.Value.size());
        for (const char character : m_Lease->Id.Value)
        {
            payload.push_back(static_cast<std::byte>(character));
        }

        const auto sendResult = m_Sender.Send(::PiSubmarine::Udp::Api::Datagram{
            .Peer = m_ServerEndpoint,
            .Payload = std::move(payload)});

        if (!sendResult.has_value())
        {
            m_NextSubscribeAttempt = uptime + m_SubscribeRetryInterval;
            return;
        }

        m_IsSubscriptionPending = false;
    }

    void Client::ReceiveDatagrams()
    {
        while (true)
        {
            const auto receiveResult = m_Receiver.TryReceive();
            if (!receiveResult.has_value() || !receiveResult->has_value())
            {
                return;
            }

            const auto deserializeResult = m_Deserializer.Deserialize(receiveResult->value().Payload);
            if (!deserializeResult.has_value())
            {
                continue;
            }

            m_Snapshot = *deserializeResult;
        }
    }
}
