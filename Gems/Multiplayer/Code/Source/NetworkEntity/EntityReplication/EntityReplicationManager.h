/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <Source/NetworkEntity/EntityReplication/EntityReplicator.h>
#include <Multiplayer/Components/NetBindComponent.h>
#include <Multiplayer/EntityDomains/IEntityDomain.h>
#include <Multiplayer/NetworkEntity/INetworkEntityManager.h>
#include <Multiplayer/NetworkEntity/NetworkEntityHandle.h>
#include <Multiplayer/NetworkEntity/NetworkEntityUpdateMessage.h>
#include <Multiplayer/NetworkEntity/NetworkEntityRpcMessage.h>
#include <Multiplayer/ReplicationWindows/IReplicationWindow.h>
#include <AzNetworking/DataStructures/TimeoutQueue.h>
#include <AzNetworking/PacketLayer/IPacketHeader.h>
#include <AzCore/std/containers/map.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/containers/deque.h>
#include <AzCore/std/limits.h>
#include <AzCore/EBus/Event.h>
#include <AzCore/EBus/ScheduledEvent.h>

namespace AzNetworking
{
    class IConnection;
    class IConnectionListener;
}

namespace Multiplayer
{
    class IEntityDomain;
    class EntityReplicator;
    
    //! @class EntityReplicationManager
    //! @brief Handles replication of relevant entities for one connection.
    class EntityReplicationManager final
    {
    public:
        using EntityReplicatorMap = AZStd::map<NetEntityId, AZStd::unique_ptr<EntityReplicator>>;

        enum class Mode
        {
            Invalid,
            LocalServerToRemoteClient,
            LocalServerToRemoteServer,
            LocalClientToRemoteServer,
        };

        EntityReplicationManager(AzNetworking::IConnection& connection, AzNetworking::IConnectionListener& connectionListener, Mode mode);
        ~EntityReplicationManager() = default;

        void SetRemoteHostId(HostId hostId);
        HostId GetRemoteHostId() const;

        void ActivatePendingEntities();
        void SendUpdates(AZ::TimeMs hostTimeMs);
        void Clear(bool forMigration);

        bool SetEntityRebasing(NetworkEntityHandle& entityHandle);

        void MigrateAllEntities();
        void MigrateEntity(NetEntityId netEntityId);
        bool CanMigrateEntity(const ConstNetworkEntityHandle& entityHandle) const;

        bool HasRemoteAuthority(const ConstNetworkEntityHandle& entityHandle) const;

        void SetEntityDomain(AZStd::unique_ptr<IEntityDomain> entityDomain);
        IEntityDomain* GetEntityDomain();
        void SetReplicationWindow(AZStd::unique_ptr<IReplicationWindow> replicationWindow);
        IReplicationWindow* GetReplicationWindow();

        void GetEntityReplicatorIdList(AZStd::list<NetEntityId>& outList);
        uint32_t GetEntityReplicatorCount(NetEntityRole localNetworkRole);

        void AddDeferredRpcMessage(NetworkEntityRpcMessage& rpcMessage);

        void AddAutonomousEntityReplicatorCreatedHandle(AZ::Event<NetEntityId>::Handler& handler);

        bool HandleEntityMigration(AzNetworking::IConnection* invokingConnection, EntityMigrationMessage& message);
        bool HandleEntityDeleteMessage(EntityReplicator* entityReplicator, const AzNetworking::IPacketHeader& packetHeader, const NetworkEntityUpdateMessage& updateMessage);
        bool HandleEntityUpdateMessage(AzNetworking::IConnection* invokingConnection, const AzNetworking::IPacketHeader& packetHeader, const NetworkEntityUpdateMessage& updateMessage);
        bool HandleEntityRpcMessage(AzNetworking::IConnection* invokingConnection, NetworkEntityRpcMessage& message);

        AZ::TimeMs GetResendTimeoutTimeMs() const;

        void SetMaxRemoteEntitiesPendingCreationCount(uint32_t maxPendingEntities);
        void SetEntityActivationTimeSliceMs(AZ::TimeMs timeSliceMs);
        void SetEntityPendingRemovalMs(AZ::TimeMs entityPendingRemovalMs);

        AzNetworking::IConnection& GetConnection();
        AZ::TimeMs GetFrameTimeMs();

        void AddReplicatorToPendingSend(const EntityReplicator& entityReplicator);
        
        bool IsUpdateModeToServerClient();

    private:
        AZ_DISABLE_COPY_MOVE(EntityReplicationManager);

        enum class UpdateValidationResult
        {
            HandleMessage,            // Handle an entity update message
            DropMessage,              // Do not handle an entity update message, but don't disconnect (could be out of order/date and isn't relevant)
            DropMessageAndDisconnect, // Do not handle the message, it is malformed and we should disconnect the connection
        };

        UpdateValidationResult ValidateUpdate(const NetworkEntityUpdateMessage& updateMessage, AzNetworking::PacketId packetId, EntityReplicator* entityReplicator);

        using RpcMessages = AZStd::list<NetworkEntityRpcMessage>;
        bool DispatchOrphanedRpc(NetworkEntityRpcMessage& message, EntityReplicator* entityReplicator);

        using EntityReplicatorList = AZStd::deque<EntityReplicator*>;
        EntityReplicatorList GenerateEntityUpdateList();

        void SendEntityUpdatesPacketHelper(AZ::TimeMs hostTimeMs, EntityReplicatorList& toSendList, uint32_t maxPayloadSize, AzNetworking::IConnection& connection);

        void SendEntityUpdates(AZ::TimeMs hostTimeMs);
        void SendEntityRpcs(RpcMessages& deferredRpcs, bool reliable);

        void MigrateEntityInternal(NetEntityId entityId);
        void OnEntityExitDomain(const ConstNetworkEntityHandle& entityHandle);
        void OnPostEntityMigration(const ConstNetworkEntityHandle& entityHandle, HostId remoteHostId, AzNetworking::ConnectionId connectionId);

        EntityReplicator* AddEntityReplicator(const ConstNetworkEntityHandle& entityHandle, NetEntityRole netEntityRole);

        const EntityReplicator* GetEntityReplicator(NetEntityId entityId) const;
        EntityReplicator* GetEntityReplicator(NetEntityId entityId);
        EntityReplicator* GetEntityReplicator(const ConstNetworkEntityHandle& entityHandle);

        void UpdateWindow();

        bool HandlePropertyChangeMessage
        (
            EntityReplicator* entityReplicator,
            AzNetworking::PacketId packetId,
            NetEntityId netEntityId,
            NetEntityRole netEntityRole,
            AzNetworking::ISerializer& serializer,
            const PrefabEntityId& prefabEntityId
        );

        void AddReplicatorToPendingRemoval(const EntityReplicator& replicator);
        void ClearRemovedReplicators();

        class OrphanedEntityRpcs
            : public AzNetworking::ITimeoutHandler
        {
        public:
            OrphanedEntityRpcs(EntityReplicationManager& replicationManager);
            void Update();
            bool DispatchOrphanedRpcs(EntityReplicator& entityReplicator);
            void AddOrphanedRpc(NetEntityId entityId, NetworkEntityRpcMessage& entityRpcMessage);
            AZStd::size_t Size() const { return m_entityRpcMap.size(); }
        private:
            AzNetworking::TimeoutResult HandleTimeout(AzNetworking::TimeoutQueue::TimeoutItem& item) override;
            struct OrphanedRpcs
            {
                OrphanedRpcs() = default;
                OrphanedRpcs(OrphanedRpcs&& rhs)
                {
                    m_rpcMessages.swap(rhs.m_rpcMessages);
                    m_timeoutId = rhs.m_timeoutId;
                    rhs.m_timeoutId = AzNetworking::TimeoutId{ 0 };
                }
                RpcMessages m_rpcMessages;
                AzNetworking::TimeoutId m_timeoutId = AzNetworking::TimeoutId{ 0 };
            };
            typedef AZStd::unordered_map<NetEntityId, OrphanedRpcs> EntityRpcMap;
            EntityRpcMap m_entityRpcMap;
            AzNetworking::TimeoutQueue m_timeoutQueue;
            EntityReplicationManager& m_replicationManager;
        };
        OrphanedEntityRpcs m_orphanedEntityRpcs;
        EntityReplicatorMap m_entityReplicatorMap;

        //! The set of entities that we have sent creation messages for, but have not received confirmation back that the create has occurred
        AZStd::unordered_set<NetEntityId> m_remoteEntitiesPendingCreation;
        AZStd::deque<NetEntityId> m_entitiesPendingActivation;
        AZStd::set<NetEntityId> m_replicatorsPendingRemoval;
        AZStd::unordered_set<NetEntityId> m_replicatorsPendingSend;

        // Deferred RPC Sends
        RpcMessages m_deferredRpcMessagesReliable;
        RpcMessages m_deferredRpcMessagesUnreliable;

        AZ::Event<NetEntityId> m_autonomousEntityReplicatorCreated;
        EntityExitDomainEvent::Handler m_entityExitDomainEventHandler;

        AZ::ScheduledEvent m_clearRemovedReplicators;
        AZ::ScheduledEvent m_updateWindow;

        AzNetworking::IConnectionListener& m_connectionListener;
        AzNetworking::IConnection& m_connection;
        AZStd::unique_ptr<IReplicationWindow> m_replicationWindow;
        AZStd::unique_ptr<IEntityDomain> m_remoteEntityDomain;

        AZ::TimeMs m_entityActivationTimeSliceMs = AZ::TimeMs{ 0 };
        AZ::TimeMs m_entityPendingRemovalMs = AZ::TimeMs{ 0 };
        AZ::TimeMs m_frameTimeMs = AZ::TimeMs{ 0 };
        HostId m_remoteHostId = InvalidHostId;
        uint32_t m_maxRemoteEntitiesPendingCreationCount = AZStd::numeric_limits<uint32_t>::max();
        uint32_t m_maxPayloadSize = 0;
        Mode m_updateMode = Mode::Invalid;

        friend class EntityReplicator;
    };
}

