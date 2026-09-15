#pragma once

#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <openrct2/GameState.h>
#include <openrct2/world/Location.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace OpenRCT2::Testing
{
    using json_t = nlohmann::json;

    enum class TileRole : uint8_t { track, entrance, exit, queue };

    struct TileRoleHandle
    {
        TileRole role{};
        TileCoordsXY coords{};
        uint8_t expectedType{};
        uint8_t expectedFlags{};
        uint8_t expectedBaseHeight{};
        uint8_t expectedClearanceHeight{};
        uint8_t expectedOwner{};
        uint8_t expectedDirection{};
        std::vector<uint8_t> expectedBytes;

        TileRoleHandle() = default;
        TileRoleHandle(TileRole role_, TileCoordsXY coords_)
            : role(role_)
            , coords(coords_)
        {
        }
    };

    struct CampaignHandle
    {
        uint8_t type{};
        RideId ride{ RideId::GetNull() };
    };

    struct NewsHandle
    {
        bool archived{};
        size_t slot{};
    };

    struct ProjectionFixtureHandles
    {
        RideId ride{ RideId::GetNull() };
        // linkedGuest/queueGuest remain compatibility aliases for the retained
        // S1 fixture.  The semantic handles below are the contract identities.
        EntityId linkedGuest{ EntityId::GetNull() };
        EntityId queueGuest{ EntityId::GetNull() };
        EntityId seatedGuest{ EntityId::GetNull() };
        EntityId watchingGuest{ EntityId::GetNull() };
        EntityId queueTailGuest{ EntityId::GetNull() };
        EntityId queueHeadGuest{ EntityId::GetNull() };
        EntityId vehicleHead{ EntityId::GetNull() };
        EntityId vehicleTail{ EntityId::GetNull() };
        BannerIndex banner{ BannerIndex::GetNull() };
        CampaignHandle campaign;
        NewsHandle recentNews;
        NewsHandle archivedNews;
        std::array<TileRoleHandle, 4> tiles{};
        uint16_t expectedTrackZ{};
        uint16_t expectedTrackTypeAndDirection{};
        uint32_t expectedHeadFlags{};
        uint32_t expectedTailFlags{};
    };

    struct ProjectionFixtureResult
    {
        std::optional<ProjectionFixtureHandles> handles;
        std::string failure;
    };

    enum class ProjectionFixtureMutation : uint8_t
    {
        none,
        removeLinkedGuest,
        removeTailVehicle,
        breakVehicleReciprocalLink,
        alterCampaignType,
        clearBannerLink,
        clearGuestItemFlags,
        aliasQueueAndExitTiles,
        omitRemovedEntranceWatch,
        alterQueueTime,
        alterBannerPosition,
        breakRideRing,
        moveSeatedOccupant,
        alterSeatedCurrentCar,
        putWatchingGuestInVehicle,
        alterVehiclePosition,
        alterVehicleFlags,
        alterUnusedOccupant,
        alterMeasurementMetadata,
        alterParkValue,
        alterCampaignFlags,
        alterBannerFlags,
        alterTileRole,
        breakQueueCardinality,
    };

    enum class ProjectionRecordKind : uint8_t
    {
        ride,
        vehicle,
        guest,
        news,
        banner,
        campaign,
        tile,
        finance,
        park,
        watch,
    };

    struct ProjectionRecordIdentity
    {
        ProjectionRecordKind kind{};
        uint16_t id{};
        TileRole role{};
        bool archived{};
        uint16_t slot{};

        friend bool operator==(const ProjectionRecordIdentity&, const ProjectionRecordIdentity&) = default;
    };

    enum class ProjectionField : uint16_t
    {
        id,
        exists,
        raw,
        value,
        flags,
        ride,
        type,
        subtype,
        status,
        link,
        positionX,
        positionY,
        positionZ,
        station,
        currentCar,
        occupant,
        queueLength,
        queueTime,
        parkValue,
        measurementFlags,
        measurementLastUseTick,
        measurementCurrentItem,
        measurementVehicleIndex,
        measurementStation,
        campaignFlags,
        bannerFlags,
        tileBytes,
        financeCell,
        historyCell,
        tileTyped,
    };

    using ProjectionIndex = std::array<uint16_t, 2>;

    struct ExpectedField
    {
        ProjectionRecordIdentity record;
        ProjectionField field{};
        ProjectionIndex index{};
        json_t expected;
    };

    struct ProjectionContract
    {
        std::vector<std::string> requiredPaths;
        std::vector<ExpectedField> exactFields;
    };

    struct ProjectionFieldInstance
    {
        ProjectionField field{};
        ProjectionRecordIdentity record;
        ProjectionIndex index{};

        friend bool operator==(const ProjectionFieldInstance&, const ProjectionFieldInstance&) = default;
    };

    void SetProjectionFixtureMutation(ProjectionFixtureMutation mutation);
    void ClearProjectionFixtureMutation();
    void ApplyProjectionFixtureMutation(GameState_t& state);

    // This filter is test-only and is checked before the corresponding leaf is
    // inserted.  It is deliberately typed by the concrete record and index;
    // the independent schema is not reachable from this API.
    void SetProjectionSerializerOmission(ProjectionFieldInstance field);
    void ClearProjectionSerializerOmission();

    class ProjectionSerializerOmissionScope
    {
    public:
        explicit ProjectionSerializerOmissionScope(ProjectionFieldInstance field)
        {
            SetProjectionSerializerOmission(field);
        }
        ~ProjectionSerializerOmissionScope()
        {
            ClearProjectionSerializerOmission();
        }
        ProjectionSerializerOmissionScope(const ProjectionSerializerOmissionScope&) = delete;
        ProjectionSerializerOmissionScope& operator=(const ProjectionSerializerOmissionScope&) = delete;
    };

    struct RideProjectionWatchSet
    {
        std::set<uint16_t> rideIds;
        std::set<uint16_t> vehicleIds;
        std::set<uint16_t> guestIds;
        std::vector<size_t> recentNewsIndices;
        std::vector<size_t> archivedNewsIndices;
        std::vector<uint16_t> bannerIds;
        std::vector<std::pair<uint8_t, uint16_t>> campaignKeys;
        std::vector<TileCoordsXY> tileCoords;
        std::optional<ProjectionFixtureHandles> fixtureHandles;
    };

    void SetProjectionFixtureHandles(const ProjectionFixtureHandles& handles);
    void ClearProjectionFixtureHandles();
    RideProjectionWatchSet CaptureRideProjectionWatchSet(const GameState_t& state, const json_t& args);
    void SetRideProjectionWatchSet(RideProjectionWatchSet watch);
    void ClearRideProjectionWatchSet();
    json_t SerializeRideProjection(const GameState_t& state, const json_t& args, const RideProjectionWatchSet& watch);
    json_t SerializeRideProjection(const GameState_t& state, const json_t& args);

    // Implemented by the test-side independent schema translation unit.  The
    // serializer has no access to its declarations or arrays.
    std::vector<std::string> IndependentKernelPaths(const ProjectionFixtureHandles& handles, bool measurementPresent = true);
    bool ValidateRideProjectionStores(const json_t& projection, std::string* failure = nullptr);
} // namespace OpenRCT2::Testing
