#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "PiSubmarine/Error/Api/ErrorCondition.h"
#include "PiSubmarine/Lease/Api/ILeaseIssuerMock.h"
#include "PiSubmarine/Telemetry/Client/Udp/Client.h"
#include "PiSubmarine/Telemetry/IDeserializerMock.h"
#include "PiSubmarine/Udp/Api/IReceiverMock.h"
#include "PiSubmarine/Udp/Api/ISenderMock.h"

namespace PiSubmarine::Telemetry::Client::Udp
{
    namespace
    {
        using ::testing::_;
        using ::testing::Return;
        using ::testing::StrictMock;
    }

    TEST(ClientTest, AcquiresLeaseAndSubscribesOnFirstTick)
    {
        StrictMock<Lease::Api::ILeaseIssuerMock> leaseIssuer;
        StrictMock<::PiSubmarine::Telemetry::IDeserializerMock> deserializer;
        StrictMock<::PiSubmarine::Udp::Api::IReceiverMock> receiver;
        StrictMock<::PiSubmarine::Udp::Api::ISenderMock> sender;

        Client client(
            leaseIssuer,
            deserializer,
            receiver,
            sender,
            ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000});

        EXPECT_CALL(leaseIssuer, AcquireLease(Lease::Api::LeaseRequest{
                        .Resource = Lease::Api::ResourceId{.Value = "telemetry-main"}}))
            .WillOnce(Return(Error::Api::Result<Lease::Api::Lease>(Lease::Api::Lease{
                .Id = Lease::Api::LeaseId{.Value = "lease-1"},
                .Resource = Lease::Api::ResourceId{.Value = "telemetry-main"},
                .Duration = std::chrono::milliseconds(3000)})));
        EXPECT_CALL(sender, Send(::testing::Truly([](const ::PiSubmarine::Udp::Api::Datagram& datagram)
            {
                return datagram.Peer == ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000}
                    && datagram.Payload == std::vector<std::byte>{
                        std::byte{'l'}, std::byte{'e'}, std::byte{'a'},
                        std::byte{'s'}, std::byte{'e'}, std::byte{'-'}, std::byte{'1'}};
            })))
            .WillOnce(Return(Error::Api::Result<void>{}));
        EXPECT_CALL(receiver, TryReceive())
            .WillOnce(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                std::optional<::PiSubmarine::Udp::Api::Datagram>{std::nullopt})));
        EXPECT_CALL(leaseIssuer, ReleaseLease(Lease::Api::LeaseId{.Value = "lease-1"}))
            .WillOnce(Return(Error::Api::Result<void>{}));

        client.Tick(std::chrono::seconds(1), std::chrono::milliseconds(10));

        const auto snapshot = client.GetSnapshot();
        ASSERT_FALSE(snapshot.has_value());
        EXPECT_EQ(snapshot.error().Condition, Error::Api::ErrorCondition::UnknownError);
    }

    TEST(ClientTest, RenewsLeaseAndResubscribesWhenLeaseIsNearExpiry)
    {
        StrictMock<Lease::Api::ILeaseIssuerMock> leaseIssuer;
        StrictMock<::PiSubmarine::Telemetry::IDeserializerMock> deserializer;
        StrictMock<::PiSubmarine::Udp::Api::IReceiverMock> receiver;
        StrictMock<::PiSubmarine::Udp::Api::ISenderMock> sender;

        Client client(
            leaseIssuer,
            deserializer,
            receiver,
            sender,
            ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000});

        EXPECT_CALL(leaseIssuer, AcquireLease(_))
            .WillOnce(Return(Error::Api::Result<Lease::Api::Lease>(Lease::Api::Lease{
                .Id = Lease::Api::LeaseId{.Value = "lease-1"},
                .Resource = Lease::Api::ResourceId{.Value = "telemetry-main"},
                .Duration = std::chrono::milliseconds(3000)})));
        EXPECT_CALL(sender, Send(_))
            .WillOnce(Return(Error::Api::Result<void>{}))
            .WillOnce(Return(Error::Api::Result<void>{}));
        EXPECT_CALL(receiver, TryReceive())
            .Times(2)
            .WillRepeatedly(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                std::optional<::PiSubmarine::Udp::Api::Datagram>{std::nullopt})));
        EXPECT_CALL(leaseIssuer, RenewLease(Lease::Api::LeaseId{.Value = "lease-1"}))
            .WillOnce(Return(Error::Api::Result<Lease::Api::Lease>(Lease::Api::Lease{
                .Id = Lease::Api::LeaseId{.Value = "lease-1"},
                .Resource = Lease::Api::ResourceId{.Value = "telemetry-main"},
                .Duration = std::chrono::milliseconds(3000)})));
        EXPECT_CALL(leaseIssuer, ReleaseLease(Lease::Api::LeaseId{.Value = "lease-1"}))
            .WillOnce(Return(Error::Api::Result<void>{}));

        client.Tick(std::chrono::seconds(1), std::chrono::milliseconds(10));
        client.Tick(std::chrono::milliseconds(2500), std::chrono::milliseconds(10));
    }

    TEST(ClientTest, UpdatesCachedSnapshotFromReceivedDatagram)
    {
        StrictMock<Lease::Api::ILeaseIssuerMock> leaseIssuer;
        StrictMock<::PiSubmarine::Telemetry::IDeserializerMock> deserializer;
        StrictMock<::PiSubmarine::Udp::Api::IReceiverMock> receiver;
        StrictMock<::PiSubmarine::Udp::Api::ISenderMock> sender;

        Client client(
            leaseIssuer,
            deserializer,
            receiver,
            sender,
            ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000});

        EXPECT_CALL(leaseIssuer, AcquireLease(_))
            .WillOnce(Return(Error::Api::Result<Lease::Api::Lease>(Lease::Api::Lease{
                .Id = Lease::Api::LeaseId{.Value = "lease-1"},
                .Resource = Lease::Api::ResourceId{.Value = "telemetry-main"},
                .Duration = std::chrono::milliseconds(3000)})));
        EXPECT_CALL(sender, Send(_))
            .WillOnce(Return(Error::Api::Result<void>{}));

        const ::PiSubmarine::Udp::Api::Datagram telemetryDatagram{
            .Peer = ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000},
            .Payload = {std::byte{0xAA}, std::byte{0xBB}}};
        Api::Snapshot expectedSnapshot{};
        expectedSnapshot.Depth = 3.5_m;

        EXPECT_CALL(receiver, TryReceive())
            .WillOnce(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(telemetryDatagram)))
            .WillOnce(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                std::optional<::PiSubmarine::Udp::Api::Datagram>{std::nullopt})));
        EXPECT_CALL(deserializer, Deserialize(::testing::ElementsAre(std::byte{0xAA}, std::byte{0xBB})))
            .WillOnce(Return(Error::Api::Result<Api::Snapshot>(expectedSnapshot)));
        EXPECT_CALL(leaseIssuer, ReleaseLease(Lease::Api::LeaseId{.Value = "lease-1"}))
            .WillOnce(Return(Error::Api::Result<void>{}));

        client.Tick(std::chrono::seconds(1), std::chrono::milliseconds(10));

        const auto snapshot = client.GetSnapshot();
        ASSERT_TRUE(snapshot.has_value());
        EXPECT_EQ(*snapshot, expectedSnapshot);
    }
}
