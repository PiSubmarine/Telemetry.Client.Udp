#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string_view>

#include "PiSubmarine/Error/Api/ErrorCondition.h"
#include "PiSubmarine/Lease/Api/ILeaseIssuerMock.h"
#include "PiSubmarine/Telemetry/Api/IRawCacheMock.h"
#include "PiSubmarine/Telemetry/Client/Udp/Client.h"
#include "PiSubmarine/Telemetry/Client/Udp/Source.h"
#include "PiSubmarine/Udp/Api/IReceiverMock.h"
#include "PiSubmarine/Udp/Api/ISenderMock.h"

namespace PiSubmarine::Telemetry::Client::Udp
{
    namespace
    {
        using ::testing::_;
        using ::testing::Return;
        using ::testing::StrictMock;

        [[nodiscard]] std::vector<std::byte> EncodeDatagram(
            const std::initializer_list<std::pair<std::string_view, std::vector<std::byte>>> channels)
        {
            std::vector<std::byte> bytes;
            const auto appendUInt32 = [&bytes](const std::uint32_t value)
            {
                bytes.push_back(static_cast<std::byte>((value >> 24U) & 0xFFU));
                bytes.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
                bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
                bytes.push_back(static_cast<std::byte>(value & 0xFFU));
            };

            appendUInt32(static_cast<std::uint32_t>(channels.size()));
            for (const auto& [channel, payload] : channels)
            {
                appendUInt32(static_cast<std::uint32_t>(channel.size()));
                for (const char character : channel)
                {
                    bytes.push_back(static_cast<std::byte>(character));
                }

                appendUInt32(static_cast<std::uint32_t>(payload.size()));
                bytes.insert(bytes.end(), payload.begin(), payload.end());
            }

            return bytes;
        }
    }

    TEST(ClientTest, AcquiresLeaseAndSubscribesOnFirstTick)
    {
        StrictMock<Lease::Api::ILeaseIssuerMock> leaseIssuer;
        StrictMock<::PiSubmarine::Udp::Api::IReceiverMock> receiver;
        StrictMock<::PiSubmarine::Udp::Api::ISenderMock> sender;

        Client client(
            leaseIssuer,
            receiver,
            sender,
            ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000});

        EXPECT_CALL(leaseIssuer, AcquireLease(Lease::Api::LeaseRequest{
                        .Resource = Lease::Api::ResourceId{.Value = "telemetry-main"}}))
            .WillOnce(Return(Error::Api::Result<Lease::Api::LeaseGrant>(Lease::Api::LeaseGrant{
                .Lease = Lease::Api::Lease{
                    .Id = Lease::Api::LeaseId{.Value = "lease-1"},
                    .Resource = Lease::Api::ResourceId{.Value = "telemetry-main"},
                    .Duration = std::chrono::milliseconds(3000)},
                .Secret = Lease::Api::LeaseSecret{.Value = {std::byte{0x01}}}})));
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

        const auto rawPayload = client.GetRaw(Api::ChannelId{.Value = "battery.main"});
        ASSERT_FALSE(rawPayload.has_value());
        EXPECT_EQ(rawPayload.error().Condition, Error::Api::ErrorCondition::UnknownError);
    }

    TEST(ClientTest, RenewsLeaseAndResubscribesWhenLeaseIsNearExpiry)
    {
        StrictMock<Lease::Api::ILeaseIssuerMock> leaseIssuer;
        StrictMock<::PiSubmarine::Udp::Api::IReceiverMock> receiver;
        StrictMock<::PiSubmarine::Udp::Api::ISenderMock> sender;

        Client client(
            leaseIssuer,
            receiver,
            sender,
            ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000});

        EXPECT_CALL(leaseIssuer, AcquireLease(_))
            .WillOnce(Return(Error::Api::Result<Lease::Api::LeaseGrant>(Lease::Api::LeaseGrant{
                .Lease = Lease::Api::Lease{
                    .Id = Lease::Api::LeaseId{.Value = "lease-1"},
                    .Resource = Lease::Api::ResourceId{.Value = "telemetry-main"},
                    .Duration = std::chrono::milliseconds(3000)},
                .Secret = Lease::Api::LeaseSecret{.Value = {std::byte{0x01}}}})));
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

    TEST(ClientTest, UpdatesRawPayloadCacheFromReceivedDatagram)
    {
        StrictMock<Lease::Api::ILeaseIssuerMock> leaseIssuer;
        StrictMock<::PiSubmarine::Udp::Api::IReceiverMock> receiver;
        StrictMock<::PiSubmarine::Udp::Api::ISenderMock> sender;

        Client client(
            leaseIssuer,
            receiver,
            sender,
            ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000});

        EXPECT_CALL(leaseIssuer, AcquireLease(_))
            .WillOnce(Return(Error::Api::Result<Lease::Api::LeaseGrant>(Lease::Api::LeaseGrant{
                .Lease = Lease::Api::Lease{
                    .Id = Lease::Api::LeaseId{.Value = "lease-1"},
                    .Resource = Lease::Api::ResourceId{.Value = "telemetry-main"},
                    .Duration = std::chrono::milliseconds(3000)},
                .Secret = Lease::Api::LeaseSecret{.Value = {std::byte{0x01}}}})));
        EXPECT_CALL(sender, Send(_))
            .WillOnce(Return(Error::Api::Result<void>{}));
        EXPECT_CALL(receiver, TryReceive())
            .WillOnce(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                ::PiSubmarine::Udp::Api::Datagram{
                    .Peer = ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000},
                    .Payload = EncodeDatagram({
                        {"battery.main", {std::byte{0xAA}, std::byte{0xBB}}},
                        {"motor.front-left", {std::byte{0xCC}}}})})))
            .WillOnce(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                std::optional<::PiSubmarine::Udp::Api::Datagram>{std::nullopt})));
        EXPECT_CALL(leaseIssuer, ReleaseLease(Lease::Api::LeaseId{.Value = "lease-1"}))
            .WillOnce(Return(Error::Api::Result<void>{}));

        client.Tick(std::chrono::seconds(1), std::chrono::milliseconds(10));

        const auto batteryPayload = client.GetRaw(Api::ChannelId{.Value = "battery.main"});
        ASSERT_TRUE(batteryPayload.has_value());
        EXPECT_EQ(*batteryPayload, (std::vector<std::byte>{std::byte{0xAA}, std::byte{0xBB}}));

        const auto motorPayload = client.GetRaw(Api::ChannelId{.Value = "motor.front-left"});
        ASSERT_TRUE(motorPayload.has_value());
        EXPECT_EQ(*motorPayload, (std::vector<std::byte>{std::byte{0xCC}}));
    }

    TEST(ClientTest, ReportsContractErrorForMissingChannelInLatestDatagram)
    {
        StrictMock<Lease::Api::ILeaseIssuerMock> leaseIssuer;
        StrictMock<::PiSubmarine::Udp::Api::IReceiverMock> receiver;
        StrictMock<::PiSubmarine::Udp::Api::ISenderMock> sender;

        Client client(
            leaseIssuer,
            receiver,
            sender,
            ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000});

        EXPECT_CALL(leaseIssuer, AcquireLease(_))
            .WillOnce(Return(Error::Api::Result<Lease::Api::LeaseGrant>(Lease::Api::LeaseGrant{
                .Lease = Lease::Api::Lease{
                    .Id = Lease::Api::LeaseId{.Value = "lease-1"},
                    .Resource = Lease::Api::ResourceId{.Value = "telemetry-main"},
                    .Duration = std::chrono::milliseconds(3000)},
                .Secret = Lease::Api::LeaseSecret{.Value = {std::byte{0x01}}}})));
        EXPECT_CALL(sender, Send(_))
            .WillOnce(Return(Error::Api::Result<void>{}));
        EXPECT_CALL(receiver, TryReceive())
            .WillOnce(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                ::PiSubmarine::Udp::Api::Datagram{
                    .Peer = ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000},
                    .Payload = EncodeDatagram({{"battery.main", {std::byte{0x01}}}})})))
            .WillOnce(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                std::optional<::PiSubmarine::Udp::Api::Datagram>{std::nullopt})));
        EXPECT_CALL(leaseIssuer, ReleaseLease(Lease::Api::LeaseId{.Value = "lease-1"}))
            .WillOnce(Return(Error::Api::Result<void>{}));

        client.Tick(std::chrono::seconds(1), std::chrono::milliseconds(10));

        const auto payload = client.GetRaw(Api::ChannelId{.Value = "motor.front-left"});
        ASSERT_FALSE(payload.has_value());
        EXPECT_EQ(payload.error().Condition, Error::Api::ErrorCondition::ContractError);
    }

    TEST(ClientTest, InvalidDatagramClearsCachedPayloads)
    {
        StrictMock<Lease::Api::ILeaseIssuerMock> leaseIssuer;
        StrictMock<::PiSubmarine::Udp::Api::IReceiverMock> receiver;
        StrictMock<::PiSubmarine::Udp::Api::ISenderMock> sender;

        Client client(
            leaseIssuer,
            receiver,
            sender,
            ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000});

        EXPECT_CALL(leaseIssuer, AcquireLease(_))
            .WillOnce(Return(Error::Api::Result<Lease::Api::LeaseGrant>(Lease::Api::LeaseGrant{
                .Lease = Lease::Api::Lease{
                    .Id = Lease::Api::LeaseId{.Value = "lease-1"},
                    .Resource = Lease::Api::ResourceId{.Value = "telemetry-main"},
                    .Duration = std::chrono::milliseconds(3000)},
                .Secret = Lease::Api::LeaseSecret{.Value = {std::byte{0x01}}}})));
        EXPECT_CALL(sender, Send(_))
            .WillOnce(Return(Error::Api::Result<void>{}));
        EXPECT_CALL(receiver, TryReceive())
            .WillOnce(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                ::PiSubmarine::Udp::Api::Datagram{
                    .Peer = ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000},
                    .Payload = EncodeDatagram({{"battery.main", {std::byte{0x01}}}})})))
            .WillOnce(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                ::PiSubmarine::Udp::Api::Datagram{
                    .Peer = ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000},
                    .Payload = {std::byte{0x00}, std::byte{0x00}, std::byte{0x00}}})))
            .WillOnce(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                std::optional<::PiSubmarine::Udp::Api::Datagram>{std::nullopt})));
        EXPECT_CALL(leaseIssuer, ReleaseLease(Lease::Api::LeaseId{.Value = "lease-1"}))
            .WillOnce(Return(Error::Api::Result<void>{}));

        client.Tick(std::chrono::seconds(1), std::chrono::milliseconds(10));

        const auto payload = client.GetRaw(Api::ChannelId{.Value = "battery.main"});
        ASSERT_FALSE(payload.has_value());
        EXPECT_EQ(payload.error().Condition, Error::Api::ErrorCondition::ContractError);
    }

    TEST(SourceTest, DelegatesToRawCacheForConfiguredChannel)
    {
        StrictMock<Api::IRawCacheMock> rawCache;
        Source source(rawCache, Api::ChannelId{.Value = "battery.main"});
        const std::vector<std::byte> expectedPayload{std::byte{0x11}, std::byte{0x22}};

        EXPECT_CALL(rawCache, GetRaw(Api::ChannelId{.Value = "battery.main"}))
            .WillOnce(Return(Error::Api::Result<std::vector<std::byte>>(expectedPayload)));

        const auto payload = source.GetRaw();

        ASSERT_TRUE(payload.has_value());
        EXPECT_EQ(*payload, expectedPayload);
    }
}
