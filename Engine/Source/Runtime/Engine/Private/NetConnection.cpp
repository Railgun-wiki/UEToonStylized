// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	NetConnection.cpp: Unreal connection base class.
=============================================================================*/

#include "Engine/NetConnection.h"
#include "Misc/CommandLine.h"
#include "EngineStats.h"
#include "EngineGlobals.h"
#include "UObject/Package.h"
#include "GameFramework/PlayerController.h"
#include "Engine/LevelStreaming.h"
#include "PacketHandlers/StatelessConnectHandlerComponent.h"
#include "Engine/LocalPlayer.h"
#include "UnrealEngine.h"
#include "EngineUtils.h"
#include "Misc/NetworkVersion.h"
#include "Net/UnrealNetwork.h"
#include "Net/NetworkProfiler.h"
#include "Net/DataReplication.h"
#include "Net/NetPacketNotify.h"
#include "Engine/ActorChannel.h"
#include "Engine/ChildConnection.h"
#include "Engine/VoiceChannel.h"
#include "Net/DataChannel.h"
#include "Engine/PackageMapClient.h"
#include "Engine/NetworkObjectList.h"
#include "EncryptionComponent.h"
#include "Net/PerfCountersHelpers.h"
#include "GameDelegates.h"
#include "Misc/PackageName.h"
#include "UObject/LinkerLoad.h"
#include "UObject/ObjectKey.h"
#include "UObject/UObjectIterator.h"
#include "Net/NetworkGranularMemoryLogging.h"
#include "SocketSubsystem.h"
#include "Math/NumericLimits.h"

static TAutoConsoleVariable<int32> CVarPingExcludeFrameTime( TEXT( "net.PingExcludeFrameTime" ), 0, TEXT( "Calculate RTT time between NIC's of server and client." ) );

#if !UE_BUILD_SHIPPING
static TAutoConsoleVariable<int32> CVarPingDisplayServerTime( TEXT( "net.PingDisplayServerTime" ), 0, TEXT( "Show server frame time" ) );
#endif

static TAutoConsoleVariable<int32> CVarTickAllOpenChannels( TEXT( "net.TickAllOpenChannels" ), 0, TEXT( "If nonzero, each net connection will tick all of its open channels every tick. Leaving this off will improve performance." ) );

static TAutoConsoleVariable<int32> CVarRandomizeSequence(TEXT("net.RandomizeSequence"), 1, TEXT("Randomize initial packet sequence"));

static TAutoConsoleVariable<int32> CVarMaxChannelSize(TEXT("net.MaxChannelSize"), UNetConnection::DEFAULT_MAX_CHANNEL_SIZE, TEXT("The maximum number of channels."));

#if !UE_BUILD_SHIPPING
static TAutoConsoleVariable<int32> CVarForceNetFlush(TEXT("net.ForceNetFlush"), 0, TEXT("Immediately flush send buffer when written to (helps trace packet writes - WARNING: May be unstable)."));
#endif

static TAutoConsoleVariable<int32> CVarNetDoPacketOrderCorrection(TEXT("net.DoPacketOrderCorrection"), 0, TEXT("Whether or not to try to fix 'out of order' packet sequences, by caching packets and waiting for the missing sequence."));

static TAutoConsoleVariable<int32> CVarNetPacketOrderCorrectionEnableThreshold(TEXT("net.PacketOrderCorrectionEnableThreshold"), 1, TEXT("The number of 'out of order' packet sequences that need to occur, before correction is enabled."));

static TAutoConsoleVariable<int32> CVarNetPacketOrderMaxMissingPackets(TEXT("net.PacketOrderMaxMissingPackets"), 3, TEXT("The maximum number of missed packet sequences that is allowed, before treating missing packets as lost."));

static TAutoConsoleVariable<int32> CVarNetPacketOrderMaxCachedPackets(TEXT("net.PacketOrderMaxCachedPackets"), 32, TEXT("(NOTE: Must be power of 2!) The maximum number of packets to cache while waiting for missing packet sequences, before treating missing packets as lost."));

TAutoConsoleVariable<int32> CVarNetEnableDetailedScopeCounters(TEXT("net.EnableDetailedScopeCounters"), 1, TEXT("Enables detailed networking scope cycle counters. There are often lots of these which can negatively impact performance."));

extern int32 GNetDormancyValidate;

DECLARE_CYCLE_STAT(TEXT("NetConnection SendAcks"), Stat_NetConnectionSendAck, STATGROUP_Net);
DECLARE_CYCLE_STAT(TEXT("NetConnection Tick"), Stat_NetConnectionTick, STATGROUP_Net);
DECLARE_CYCLE_STAT(TEXT("NetConnection ReceivedNak"), Stat_NetConnectionReceivedNak, STATGROUP_Net);
DECLARE_CYCLE_STAT(TEXT("NetConnection NetConnectionReceivedAcks"), Stat_NetConnectionReceivedAcks, STATGROUP_Net);

// ChannelRecord Implementation
namespace FChannelRecordImpl
{

// Push ChannelRecordEntry for packet if PacketID differs from last PacketId
static void PushPacketId(FWrittenChannelsRecord& WrittenChannelsRecord, int32 PacketId);

// Push written ChannelIndex to WrittenChanneRecord, Push new packetId if PackedIdDiffers from Last pushed entry */
static void PushChannelRecord(FWrittenChannelsRecord& WrittenChannelsRecord, int32 PacketId, int32 ChannelIndex);

// Consume all FChannelRecordEntries for the given PacketId and execute a function with the signature (void*)(int32 PacketId, uint32 Channelndex) for each entry
template<class Functor>
static void ConsumeChannelRecordsForPacket(FWrittenChannelsRecord& WrittenChannelsRecord, int32 PacketId, Functor&& Func);

// Consume all FChannelRecordEntries and execute a function with the signature (void*)(uint32 Channelndex) each entry
template<class Functor>
static void ConsumeAllChannelRecords(FWrittenChannelsRecord& WrittenChannelsRecord, Functor&& Func);

// Returns allocated size of the record
static SIZE_T CountBytes(FWrittenChannelsRecord& WrittenChannelsRecord);

};

const int32 UNetConnection::DEFAULT_MAX_CHANNEL_SIZE = 32767;
/*-----------------------------------------------------------------------------
	UNetConnection implementation.
-----------------------------------------------------------------------------*/

UNetConnection* UNetConnection::GNetConnectionBeingCleanedUp = NULL;

UNetConnection::UNetConnection(const FObjectInitializer& ObjectInitializer)
:	UPlayer(ObjectInitializer)
,	Driver				( nullptr )
,	PackageMapClass		( UPackageMapClient::StaticClass() )
,	PackageMap			( nullptr )
,	ViewTarget			( nullptr )
,   OwningActor			( nullptr )
,	MaxPacket			( 0 )
,	InternalAck			( false )
,	RemoteAddr			( nullptr )
,	MaxPacketHandlerBits ( 0 )
,	State				( USOCK_Invalid )
,	Handler()
,	StatelessConnectComponent()
,	PacketOverhead		( 0 )
,	ResponseId			( 0 )

,	QueuedBits			( 0 )
,	TickCount			( 0 )
,	LastProcessedFrame	( 0 )
,	ConnectTime			( 0.0 )

,	AllowMerge			( false )
,	TimeSensitive		( false )
,	LastOutBunch		( nullptr )
,	SendBunchHeader		( MAX_BUNCH_HEADER_BITS )

,	StatPeriod			( 1.f  )
,	BestLag				( 9999 )
,	AvgLag				( 9999 )

,	LagAcc				( 9999 )
,	BestLagAcc			( 9999 )
,	LagCount			( 0 )
,	LastTime			( 0 )
,	FrameTime			( 0 )
,	CumulativeTime		( 0 )
,	AverageFrameTime	( 0 )
,	CountedFrames		( 0 )
,	InBytes				( 0 )
,	OutBytes			( 0 )
,	InTotalBytes		( 0 )
,	OutTotalBytes		( 0 ) 
,	InPackets			( 0 )
,	OutPackets			( 0 )
,	InTotalPackets		( 0 )
,	OutTotalPackets		( 0 )
,	InBytesPerSecond	( 0 )
,	OutBytesPerSecond	( 0 )
,	InPacketsPerSecond	( 0 )
,	OutPacketsPerSecond	( 0 )
,	InTotalPacketsLost	( 0 )
,	OutTotalPacketsLost	( 0 )
,	OutTotalAcks		( 0 )
,	AnalyticsVars		()
,	NetAnalyticsData	()
,	SendBuffer			( 0 )
,	InPacketId			( -1 )
,	OutPacketId			( 0 ) // must be initialized as OutAckPacketId + 1 so loss of first packet can be detected
,	OutAckPacketId		( -1 )
,	bLastHasServerFrameTime( false )
,	InitOutReliable		( 0 )
,	InitInReliable		( 0 )
,	EngineNetworkProtocolVersion( FNetworkVersion::GetEngineNetworkProtocolVersion() )
,	GameNetworkProtocolVersion( FNetworkVersion::GetGameNetworkProtocolVersion() )
PRAGMA_DISABLE_DEPRECATION_WARNINGS
,	bResendAllDataSinceOpen( false )
PRAGMA_ENABLE_DEPRECATION_WARNINGS
,	ResendAllDataState( EResendAllDataState::None )
#if !UE_BUILD_SHIPPING
,	ReceivedRawPacketDel()
#endif
,	PlayerOnlinePlatformName( NAME_None )
,	ClientWorldPackageName( NAME_None )
,	LastNotifiedPacketId( -1 )
,	OutTotalNotifiedPackets(0)
,	HasDirtyAcks(0u)
,	bHasWarnedAboutChannelLimit(false)
,	bConnectionPendingCloseDueToSocketSendFailure(false)
,	TotalOutOfOrderPackets(0)
,	PacketOrderCache()
,	PacketOrderCacheStartIdx(0)
,	PacketOrderCacheCount(0)
,	bFlushingPacketOrderCache(false)
{
	// This isn't ideal, because it won't capture memory derived classes are creating dynamically.
	// The allocations could *probably* be moved somewhere else (like InitBase), but that
	// causes failure to connect for some reason, and for now this is easier.
	LLM_SCOPE(ELLMTag::Networking);

	MaxChannelSize = CVarMaxChannelSize.GetValueOnAnyThread();
	if (MaxChannelSize <= 0)
	{
		UE_LOG(LogNet, Warning, TEXT("CVarMaxChannelSize of %d is less than or equal to 0, using the default number of channels."), MaxChannelSize);
		MaxChannelSize = DEFAULT_MAX_CHANNEL_SIZE;
	}
	
	
	if (!HasAnyFlags(EObjectFlags::RF_ClassDefaultObject | EObjectFlags::RF_ArchetypeObject))
	{
		Channels.AddDefaulted(MaxChannelSize);
		OutReliable.AddDefaulted(MaxChannelSize);
		InReliable.AddDefaulted(MaxChannelSize);
		PendingOutRec.AddDefaulted(MaxChannelSize);

		PacketNotify.Init(InPacketId, OutPacketId);
	}	
}

/**
 * Initialize common settings for this connection instance
 *
 * @param InDriver the net driver associated with this connection
 * @param InSocket the socket associated with this connection
 * @param InURL the URL to init with
 * @param InState the connection state to start with for this connection
 * @param InMaxPacket the max packet size that will be used for sending
 * @param InPacketOverhead the packet overhead for this connection type
 */
void UNetConnection::InitBase(UNetDriver* InDriver,class FSocket* InSocket, const FURL& InURL, EConnectionState InState, int32 InMaxPacket, int32 InPacketOverhead)
{
	// Oodle depends upon this
	check(InMaxPacket <= MAX_PACKET_SIZE);

	// Owning net driver
	Driver = InDriver;

	// Stats
	StatUpdateTime			= Driver->Time;
	LastReceiveTime			= Driver->Time;
	LastReceiveRealtime		= 0.0;			// These are set to 0 and initialized on our first tick to deal with scenarios where
	LastGoodPacketRealtime	= 0.0;			// notable time may elapse between init and first use
	LastTime				= 0.0;
	LastSendTime			= Driver->Time;
	LastTickTime			= Driver->Time;
	LastRecvAckTime			= Driver->Time;
	ConnectTime				= Driver->Time;

	// Analytics
	TSharedPtr<FNetAnalyticsAggregator>& AnalyticsAggregator = Driver->AnalyticsAggregator;

	if (AnalyticsAggregator.IsValid())
	{
		NetAnalyticsData = REGISTER_NET_ANALYTICS(AnalyticsAggregator, FNetConnAnalyticsData, TEXT("Core.ServerNetConn"));
	}

	NetConnectionHistogram.InitHitchTracking();

	// Current state
	State = InState;
	// Copy the URL
	URL = InURL;

	// Use the passed in values
	MaxPacket = InMaxPacket;
	PacketOverhead = InPacketOverhead;

	check(MaxPacket > 0 && PacketOverhead > 0);


	// Reset Handler
	Handler.Reset(NULL);

	InitHandler();

#if DO_ENABLE_NET_TEST
	// Copy the command line settings from the net driver
	UpdatePacketSimulationSettings();
#endif

	// Other parameters.
	CurrentNetSpeed = URL.HasOption(TEXT("LAN")) ? GetDefault<UPlayer>()->ConfiguredLanSpeed : GetDefault<UPlayer>()->ConfiguredInternetSpeed;

	if ( CurrentNetSpeed == 0 )
	{
		CurrentNetSpeed = 2600;
	}
	else
	{
		CurrentNetSpeed = FMath::Max<int32>(CurrentNetSpeed, 1800);
	}

	// Create package map.
	UPackageMapClient* PackageMapClient = NewObject<UPackageMapClient>(this, PackageMapClass);

	if (ensure(PackageMapClient != nullptr))
	{
		PackageMapClient->Initialize(this, Driver->GuidCache);
		PackageMap = PackageMapClient;
	}
}

/**
 * Initializes an "addressless" connection with the passed in settings
 *
 * @param InDriver the net driver associated with this connection
 * @param InState the connection state to start with for this connection
 * @param InURL the URL to init with
 * @param InConnectionSpeed Optional connection speed override
 */
void UNetConnection::InitConnection(UNetDriver* InDriver, EConnectionState InState, const FURL& InURL, int32 InConnectionSpeed, int32 InMaxPacket)
{
	Driver = InDriver;

	// We won't be sending any packets, so use a default size
	MaxPacket = (InMaxPacket == 0 || InMaxPacket > MAX_PACKET_SIZE) ? MAX_PACKET_SIZE : InMaxPacket;
	PacketOverhead = 0;
	State = InState;

#if DO_ENABLE_NET_TEST
	// Copy the command line settings from the net driver
	UpdatePacketSimulationSettings();
#endif

	// Get the 
	if (InConnectionSpeed)
	{
		CurrentNetSpeed = InConnectionSpeed;
	}
	else
	{

		CurrentNetSpeed =  URL.HasOption(TEXT("LAN")) ? GetDefault<UPlayer>()->ConfiguredLanSpeed : GetDefault<UPlayer>()->ConfiguredInternetSpeed;
		if ( CurrentNetSpeed == 0 )
		{
			CurrentNetSpeed = 2600;
		}
		else
		{
			CurrentNetSpeed = FMath::Max<int32>(CurrentNetSpeed, 1800);
		}
	}

	// Create package map.
	auto PackageMapClient = NewObject<UPackageMapClient>(this);
	PackageMapClient->Initialize(this, Driver->GuidCache);
	PackageMap = PackageMapClient;
}

void UNetConnection::InitHandler()
{
	check(!Handler.IsValid());

#if !UE_BUILD_SHIPPING
	if (!FParse::Param(FCommandLine::Get(), TEXT("NoPacketHandler")))
#endif
	{
		Handler = MakeUnique<PacketHandler>();

		if (Handler.IsValid())
		{
			Handler::Mode Mode = Driver->ServerConnection != nullptr ? Handler::Mode::Client : Handler::Mode::Server;

			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			Handler->InitializeAddressSerializer([this](const FString& InAddress){
				return Driver->GetSocketSubsystem()->GetAddressFromString(InAddress);
			});
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
			Handler->InitializeDelegates(FPacketHandlerLowLevelSendTraits::CreateUObject(this, &UNetConnection::LowLevelSend));
			Handler->NotifyAnalyticsProvider(Driver->AnalyticsProvider, Driver->AnalyticsAggregator);
			Handler->Initialize(Mode, MaxPacket * 8, false, nullptr, nullptr, Driver->NetDriverName);


			// Add handling for the stateless connect handshake, for connectionless packets, as the outermost layer
			TSharedPtr<HandlerComponent> NewComponent =
				Handler->AddHandler(TEXT("Engine.EngineHandlerComponentFactory(StatelessConnectHandlerComponent)"), true);

			StatelessConnectComponent = StaticCastSharedPtr<StatelessConnectHandlerComponent>(NewComponent);

			if (StatelessConnectComponent.IsValid())
			{
				StatelessConnectComponent.Pin()->SetDriver(Driver);
			}


			Handler->InitializeComponents();

			MaxPacketHandlerBits = Handler->GetTotalReservedPacketBits();
		}
	}


#if !UE_BUILD_SHIPPING
	uint32 MaxPacketBits = MaxPacket * 8;
	uint32 ReservedTotal = MaxPacketHandlerBits + MAX_PACKET_HEADER_BITS + MAX_PACKET_TRAILER_BITS;

	SET_DWORD_STAT(STAT_MaxPacket, MaxPacketBits);
	SET_DWORD_STAT(STAT_MaxPacketMinusReserved, MaxPacketBits - ReservedTotal);
	SET_DWORD_STAT(STAT_PacketReservedTotal, ReservedTotal);
	SET_DWORD_STAT(STAT_PacketReservedNetConnection, MAX_PACKET_HEADER_BITS + MAX_PACKET_TRAILER_BITS);
	SET_DWORD_STAT(STAT_PacketReservedPacketHandler, MaxPacketHandlerBits);
#endif
}

void UNetConnection::InitSequence(int32 IncomingSequence, int32 OutgoingSequence)
{
	// Make sure the sequence hasn't already been initialized on the server, and ignore multiple initializations on the client
	check(InPacketId == -1 || Driver->ServerConnection != nullptr);

	if (InPacketId == -1 && CVarRandomizeSequence.GetValueOnAnyThread() > 0)
	{
		// Initialize the base UNetConnection packet sequence (not very useful/effective at preventing attacks)
		InPacketId = IncomingSequence - 1;
		OutPacketId = OutgoingSequence;
		OutAckPacketId = OutgoingSequence - 1;
		LastNotifiedPacketId = OutAckPacketId;

		// Initialize the reliable packet sequence (more useful/effective at preventing attacks)
		InitInReliable = IncomingSequence & (MAX_CHSEQUENCE - 1);
		InitOutReliable = OutgoingSequence & (MAX_CHSEQUENCE - 1);

		InReliable.Init(InitInReliable, InReliable.Num());
		OutReliable.Init(InitOutReliable, OutReliable.Num());

		PacketNotify.Init(InPacketId, OutPacketId);

		UE_LOG(LogNet, Verbose, TEXT("InitSequence: IncomingSequence: %i, OutgoingSequence: %i, InitInReliable: %i, InitOutReliable: %i"), IncomingSequence, OutgoingSequence, InitInReliable, InitOutReliable);
	}
}

void UNetConnection::NotifyAnalyticsProvider()
{
	if (Handler.IsValid())
	{
		Handler->NotifyAnalyticsProvider(Driver->AnalyticsProvider, Driver->AnalyticsAggregator);
	}
}

void UNetConnection::EnableEncryptionWithKey(TArrayView<const uint8> Key)
{
	if (Handler.IsValid())
	{
		UE_LOG(LogNet, Verbose, TEXT("UNetConnection::EnableEncryptionWithKey, %s"), *Describe());

		TSharedPtr<FEncryptionComponent> EncryptionComponent = Handler->GetEncryptionComponent();
		if (EncryptionComponent.IsValid())
		{
			EncryptionComponent->SetEncryptionKey(Key);
			EncryptionComponent->EnableEncryption();
		}
		else
		{
			UE_LOG(LogNet, Warning, TEXT("UNetConnection::EnableEncryptionWithKey, encryption component not found!"));
		}
	}
}

void UNetConnection::EnableEncryptionWithKeyServer(TArrayView<const uint8> Key)
{
	if (State != USOCK_Invalid && State != USOCK_Closed && Driver)
	{
		SendClientEncryptionAck();
		EnableEncryptionWithKey(Key);
	}
	else
	{
		UE_LOG(LogNet, Log, TEXT("UNetConnection::EnableEncryptionWithKeyServer, connection in invalid state. %s"), *Describe());
	}
}

void UNetConnection::SendClientEncryptionAck()
{
	if (State != USOCK_Invalid && State != USOCK_Closed && Driver)
	{
		FNetControlMessage<NMT_EncryptionAck>::Send(this);
		FlushNet();
	}
	else
	{
		UE_LOG(LogNet, Log, TEXT("UNetConnection::SendClientEncryptionAck, connection in invalid state. %s"), *Describe());
	}
}

void UNetConnection::SetEncryptionKey(TArrayView<const uint8> Key)
{
	if (Handler.IsValid())
	{
		UE_LOG(LogNet, Verbose, TEXT("UNetConnection::SetEncryptionKey, %s"), *Describe());

		TSharedPtr<FEncryptionComponent> EncryptionComponent = Handler->GetEncryptionComponent();
		if (EncryptionComponent.IsValid())
		{
			EncryptionComponent->SetEncryptionKey(Key);
		}
		else
		{
			UE_LOG(LogNet, Warning, TEXT("UNetConnection::SetEncryptionKey, encryption component not found!"));
		}
	}
}

void UNetConnection::EnableEncryption()
{
	if (Handler.IsValid())
	{
		UE_LOG(LogNet, Verbose, TEXT("UNetConnection::EnableEncryption, %s"), *Describe());

		TSharedPtr<FEncryptionComponent> EncryptionComponent = Handler->GetEncryptionComponent();
		if (EncryptionComponent.IsValid())
		{
			EncryptionComponent->EnableEncryption();
		}
		else
		{
			UE_LOG(LogNet, Warning, TEXT("UNetConnection::EnableEncryption, encryption component not found!"));
		}
	}
}

bool UNetConnection::IsEncryptionEnabled() const
{
	if (Handler.IsValid())
	{
		TSharedPtr<FEncryptionComponent> EncryptionComponent = Handler->GetEncryptionComponent();
		if (EncryptionComponent.IsValid())
		{
			return EncryptionComponent->IsEncryptionEnabled();
		}
	}

	return false;
}

void UNetConnection::Serialize( FArchive& Ar )
{
	UObject::Serialize( Ar );
	Ar << PackageMap;
	for (UChannel* Channel : Channels)
	{
		Ar << Channel;
	}

	if (Ar.IsCountingMemory())
	{
		// TODO: We don't currently track:
		//		StatelessConnectComponents
		//		AnalyticsVars
		//		AnalyticsData
		//		Histogram data.
		// These are probably insignificant, though.

		GRANULAR_NETWORK_MEMORY_TRACKING_INIT(Ar, "UNetConnection::Serialize");

		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("Challenge", Challenge.CountBytes(Ar));
		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("ClientResponse", ClientResponse.CountBytes(Ar));
		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("RequestURL", RequestURL.CountBytes(Ar));

PRAGMA_DISABLE_DEPRECATION_WARNINGS
		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("CDKeyHash", CDKeyHash.CountBytes(Ar));
		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("CDKeyResponse", CDKeyResponse.CountBytes(Ar));
PRAGMA_ENABLE_DEPRECATION_WARNINGS

		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("SendBuffer", SendBuffer.CountMemory(Ar));

		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("Channels", Channels.CountBytes(Ar));
		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("OutReliable", OutReliable.CountBytes(Ar));
		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("InReliable", InReliable.CountBytes(Ar));
		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("PendingOutRec", PendingOutRec.CountBytes(Ar));
		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("ActorChannels", ActorChannels.CountBytes(Ar));
		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("DestroyedStartupOrDormantActorGUIDs", DestroyedStartupOrDormantActorGUIDs.CountBytes(Ar));

		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("KeepProcessingActorChannelBunchesMap",
			KeepProcessingActorChannelBunchesMap.CountBytes(Ar);
			for (const auto& KeepProcessingActorChannelBunchesPair : KeepProcessingActorChannelBunchesMap)
			{
				KeepProcessingActorChannelBunchesPair.Value.CountBytes(Ar);
			}
		);

		// ObjectReplicators are going to be counted by UNetDriver::Serialize AllOwnedReplicators.
		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("DormantReplicatorMap", DormantReplicatorMap.CountBytes(Ar));

		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("ClientVisibleLevelNames", ClientVisibleLevelNames.CountBytes(Ar));
		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("ClientVisibileActorOuters", ClientVisibileActorOuters.CountBytes(Ar));

		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("ActorsStarvedByClassTimeMap",
			ActorsStarvedByClassTimeMap.CountBytes(Ar);
			for (auto& ActorsStarvedByClassTimePair : ActorsStarvedByClassTimeMap)
			{
				Ar << ActorsStarvedByClassTimePair.Key;
				ActorsStarvedByClassTimePair.Value.CountBytes(Ar);
			}
		);

		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("IgnoringChannels", IgnoringChannels.CountBytes(Ar));
		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("OutgoingBunches", OutgoingBunches.CountBytes(Ar));
		
		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("ChannelRecord",
			const SIZE_T SizeAllocatedByChannelRecord = FChannelRecordImpl::CountBytes(ChannelRecord);
			Ar.CountBytes(SizeAllocatedByChannelRecord, SizeAllocatedByChannelRecord)
		);

		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("LastOut", LastOut.CountMemory(Ar));
		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("SendBunchHeader", SendBunchHeader.CountMemory(Ar));

		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("PacketHandler",
			if (Handler.IsValid())
			{
				// PacketHandler already counts its size.
				Handler->CountBytes(Ar);
			}
		);

#if DO_ENABLE_NET_TEST
		GRANULAR_NETWORK_MEMORY_TRACKING_TRACK("Delayed",
			Delayed.CountBytes(Ar);
			for (const DelayedPacket& Packet : Delayed)
			{
				Packet.CountBytes(Ar);
			}
		);
#endif
	}
}

void UNetConnection::Close()
{
	if (Driver != nullptr && State != USOCK_Closed)
	{
		NETWORK_PROFILER(GNetworkProfiler.TrackEvent(TEXT("CLOSE"), *(GetName() + TEXT(" ") + LowLevelGetRemoteAddress()), this));
		UE_LOG(LogNet, Log, TEXT("UNetConnection::Close: %s, Channels: %i, Time: %s"), *Describe(), OpenChannels.Num(), *FDateTime::UtcNow().ToString(TEXT("%Y.%m.%d-%H.%M.%S")));

		if (Channels[0] != nullptr)
		{
			Channels[0]->Close(EChannelCloseReason::Destroyed);
		}
		State = USOCK_Closed;

		if ((Handler == nullptr || Handler->IsFullyInitialized()) && HasReceivedClientPacket())
		{
			FlushNet();
		}

		if (NetAnalyticsData.IsValid())
		{
			NetAnalyticsData->CommitAnalytics(AnalyticsVars);
		}
	}

	LogCallLastTime		= 0;
	LogCallCount		= 0;
	LogSustainedCount	= 0;
}

FString UNetConnection::Describe()
{
	return FString::Printf( TEXT( "[UNetConnection] RemoteAddr: %s, Name: %s, Driver: %s, IsServer: %s, PC: %s, Owner: %s, UniqueId: %s" ),
			*LowLevelGetRemoteAddress( true ),
			*GetName(),
			Driver ? *Driver->GetDescription() : TEXT( "NULL" ),
			Driver && Driver->IsServer() ? TEXT( "YES" ) : TEXT( "NO" ),
			PlayerController ? *PlayerController->GetName() : TEXT( "NULL" ),
			OwningActor ? *OwningActor->GetName() : TEXT( "NULL" ),
			*PlayerId.ToDebugString());
}

void UNetConnection::CleanUp()
{
	// Remove UChildConnection(s)
	for (int32 i = 0; i < Children.Num(); i++)
	{
		Children[i]->CleanUp();
	}
	Children.Empty();

	if ( State != USOCK_Closed )
	{
		UE_LOG( LogNet, Log, TEXT( "UNetConnection::Cleanup: Closing open connection. %s" ), *Describe() );
	}

	Close();

	if (Driver != nullptr)
	{
		// Remove from driver.
		if (Driver->ServerConnection)
		{
			check(Driver->ServerConnection == this);
			Driver->ServerConnection = NULL;
		}
		else
		{
			check(Driver->ServerConnection == NULL);
			Driver->RemoveClientConnection(this);

#if USE_SERVER_PERF_COUNTERS
			if (IPerfCountersModule::IsAvailable())
			{
				PerfCountersIncrement(TEXT("RemovedConnections"));
			}
#endif
		}
	}

	// Kill all channels.
	for (int32 i = OpenChannels.Num() - 1; i >= 0; i--)
	{
		UChannel* OpenChannel = OpenChannels[i];
		if (OpenChannel != NULL)
		{
			OpenChannel->ConditionalCleanUp(true, EChannelCloseReason::Destroyed);
		}
	}

	// Cleanup any straggler KeepProcessingActorChannelBunchesMap channels
	for (const TPair<FNetworkGUID, TArray<UActorChannel*>>& MapKeyValuePair : KeepProcessingActorChannelBunchesMap)
	{
		for (UActorChannel* CurChannel : MapKeyValuePair.Value)
		{
			CurChannel->ConditionalCleanUp(true, EChannelCloseReason::Destroyed);
		}
	}

	KeepProcessingActorChannelBunchesMap.Empty();

	PackageMap = NULL;

	if (GIsRunning)
	{
		if (OwningActor != NULL)
		{	
			// Cleanup/Destroy the connection actor & controller
			if (!OwningActor->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
			{
				// UNetConnection::CleanUp can be called from UNetDriver::FinishDestroyed that is called from GC.
				OwningActor->OnNetCleanup(this);
			}
			OwningActor = NULL;
			PlayerController = NULL;
		}
		else
		{
			if (ClientLoginState < EClientLoginState::ReceivedJoin)
			{
				UE_LOG(LogNet, Log, TEXT("UNetConnection::PendingConnectionLost. %s bPendingDestroy=%d "), *Describe(), bPendingDestroy);
				FGameDelegates::Get().GetPendingConnectionLostDelegate().Broadcast(PlayerId);
			}
		}
	}

	CleanupDormantActorState();

	Handler.Reset(NULL);

	SetClientLoginState(EClientLoginState::CleanedUp);

	Driver = nullptr;
}

UChildConnection::UChildConnection(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UChildConnection::CleanUp()
{
	if (GIsRunning)
	{
		if (OwningActor != NULL)
		{
			if ( !OwningActor->HasAnyFlags( RF_BeginDestroyed | RF_FinishDestroyed ) )
			{
				// Cleanup/Destroy the connection actor & controller	
				OwningActor->OnNetCleanup(this);
			}

			OwningActor = NULL;
			PlayerController = NULL;
		}
	}
	PackageMap = NULL;
	Driver = NULL;
}

void UNetConnection::FinishDestroy()
{
	if ( !HasAnyFlags(RF_ClassDefaultObject) )
	{
		CleanUp();
	}

	Super::FinishDestroy();
}

void UNetConnection::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	UNetConnection* This = CastChecked<UNetConnection>(InThis);

	// Let GC know that we're referencing some UChannel objects
	for (UChannel* Channel : This->Channels)
	{
		Collector.AddReferencedObject( Channel, This );
	}

	// Let GC know that we're referencing some UActorChannel objects
	for ( auto It = This->KeepProcessingActorChannelBunchesMap.CreateIterator(); It; ++It )
	{
		const TArray<UActorChannel*>& ChannelArray = It.Value();
		for ( UActorChannel* CurChannel : ChannelArray )
		{
			Collector.AddReferencedObject( CurChannel, This );
		}
	}

	// ClientVisibileActorOuters acceleration map
	for (auto& MapIt : This->ClientVisibileActorOuters)
	{
		Collector.AddReferencedObject(MapIt.Key, This);
	}

	Super::AddReferencedObjects(This, Collector);
}

UWorld* UNetConnection::GetWorld() const
{
	UWorld* World = nullptr;
	if (Driver)
	{
		World = Driver->GetWorld();
	}

	if (!World && OwningActor)
	{
		World = OwningActor->GetWorld();
	}

	return World;
}

bool UNetConnection::Exec( UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar )
{
	if ( Super::Exec( InWorld, Cmd,Ar) )
	{
		return true;
	}
	else if ( GEngine->Exec( InWorld, Cmd, Ar ) )
	{
		return true;
	}
	return false;
}
void UNetConnection::AssertValid()
{
	// Make sure this connection is in a reasonable state.
	check(State==USOCK_Closed || State==USOCK_Pending || State==USOCK_Open);

}

bool UNetConnection::ClientHasInitializedLevelFor(const AActor* TestActor) const
{
	checkSlow(Driver);
	checkSlow(Driver->IsServer());

	// This function is called a lot, basically for every replicated actor every time it replicates, on every client connection
	// Each client connection has a different visibility state (what levels are currently loaded for them).
	// Actor's outer is what we need

	// Note: we are calling GetOuter() here instead of GetLevel() to avoid an unreal Cast<>: we justt need the memory address for the lookup.
	UObject* ActorOuter = TestActor->GetOuter();
	if (const bool* bIsVisible = ClientVisibileActorOuters.Find(ActorOuter))
	{
		return *bIsVisible;
	}

	// The actor's outer was not in the acceleration map so we perform the "legacy" function and 
	// cache the result so that we don't do this every time:
	return UpdateCachedLevelVisibility(Cast<ULevel>(ActorOuter));
}

bool UNetConnection::UpdateCachedLevelVisibility(ULevel* Level) const
{
	bool IsVisibile = false;
	if (Level == nullptr)
	{
		IsVisibile = true;
	}
	else if (Level->IsPersistentLevel() && Driver->GetWorldPackage()->GetFName() == ClientWorldPackageName)
	{
		IsVisibile = true;
	}
	else
	{
		IsVisibile = ClientVisibleLevelNames.Contains(Level->GetOutermost()->GetFName());
	}

	ClientVisibileActorOuters.FindOrAdd(Level) = IsVisibile;
	return IsVisibile;
}

void UNetConnection::UpdateAllCachedLevelVisibility() const
{
	// Update our acceleration map
	for (auto& MapIt : ClientVisibileActorOuters)
	{
		if (ULevel* Level = Cast<ULevel>(MapIt.Key))
		{
			UpdateCachedLevelVisibility(Level);
		}
	}
}

void UNetConnection::UpdateLevelVisibility(const FName& PackageName, bool bIsVisible)
{
	GNumClientUpdateLevelVisibility++;

	// add or remove the level package name from the list, as requested
	if (bIsVisible)
	{
		// verify that we were passed a valid level name
		FString Filename;
		UPackage* TempPkg = FindPackage(nullptr, *PackageName.ToString());
		FLinkerLoad* Linker = FLinkerLoad::FindExistingLinkerForPackage(TempPkg);

		// If we have a linker we know it has been loaded off disk successfully
		// If we have a file it is fine too
		// If its in our own streaming level list, its good

		struct Local
		{
			static bool IsInLevelList(UWorld* World, FName InPackageName)
			{
				for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
				{
					if (StreamingLevel && (StreamingLevel->GetWorldAssetPackageFName() == InPackageName ))
					{
						return true;
					}
				}
				return false;
			}
		};

		if ( Linker || FPackageName::DoesPackageExist(PackageName.ToString(), nullptr, &Filename ) || Local::IsInLevelList(GetWorld(), PackageName ) )
		{
			ClientVisibleLevelNames.Add(PackageName);
			UE_LOG( LogPlayerController, Verbose, TEXT("ServerUpdateLevelVisibility() Added '%s'"), *PackageName.ToString() );

			QUICK_USE_CYCLE_STAT(NetUpdateLevelVisibility_UpdateDormantActors, STATGROUP_Net);

			// Any destroyed actors that were destroyed prior to the streaming level being unloaded for the client will not be in the connections
			// destroyed actors list when the level is reloaded, so seek them out and add in
			for (const auto& DestroyedPair : Driver->DestroyedStartupOrDormantActors)
			{
				if (DestroyedPair.Value->StreamingLevelName == PackageName)
				{
					AddDestructionInfo(DestroyedPair.Value.Get());
				}
			}

			// Any dormant actor that has changes flushed or made before going dormant needs to be updated on the client 
			// when the streaming level is loaded, so mark them active for this connection
			UWorld* LevelWorld = nullptr;
			if (TempPkg)
			{
				LevelWorld = (UWorld*)FindObjectWithOuter(TempPkg, UWorld::StaticClass());
				if (LevelWorld)
				{
					if (LevelWorld->PersistentLevel)
					{
						FNetworkObjectList& NetworkObjectList = Driver->GetNetworkObjectList();
						for (AActor* Actor : LevelWorld->PersistentLevel->Actors)
						{
							// Dormant Initial actors have no changes. Dormant Never and Awake will be sent normal, so we only need
							// to mark Dormant All Actors as (temporarily) active to get the update sent over
							if (Actor && Actor->GetIsReplicated() && (Actor->NetDormancy == DORM_DormantAll))
							{
								NetworkObjectList.MarkActive( Actor, this, Driver );
							}
						}
					}
				}
			}

			if (ReplicationConnectionDriver)
			{
				ReplicationConnectionDriver->NotifyClientVisibleLevelNamesAdd(PackageName, LevelWorld);
			}

		}
		else
		{
			UE_LOG( LogPlayerController, Warning, TEXT("ServerUpdateLevelVisibility() ignored non-existant package '%s'"), *PackageName.ToString() );
			Close();
		}
	}
	else
	{
		ClientVisibleLevelNames.Remove(PackageName);
		UE_LOG( LogPlayerController, Verbose, TEXT("ServerUpdateLevelVisibility() Removed '%s'"), *PackageName.ToString() );
		if (ReplicationConnectionDriver)
		{
			ReplicationConnectionDriver->NotifyClientVisibleLevelNamesRemove(PackageName);
		}
			
		// Close any channels now that have actors that were apart of the level the client just unloaded
		for ( auto It = ActorChannels.CreateIterator(); It; ++It )
		{
			UActorChannel* Channel = It.Value();					

			check( Channel->OpenedLocally );

			if ( Channel->Actor && Channel->Actor->GetLevel()->GetOutermost()->GetFName() == PackageName )
			{
				Channel->Close(EChannelCloseReason::LevelUnloaded);
			}
		}
	}

	UpdateAllCachedLevelVisibility();
}

void UNetConnection::SetClientWorldPackageName(FName NewClientWorldPackageName)
{
	ClientWorldPackageName = NewClientWorldPackageName;
	
	UpdateAllCachedLevelVisibility();
}

void UNetConnection::ValidateSendBuffer()
{
	if ( SendBuffer.IsError() )
	{
		UE_LOG( LogNetTraffic, Fatal, TEXT( "UNetConnection::ValidateSendBuffer: Out.IsError() == true. NumBits: %i, NumBytes: %i, MaxBits: %i" ), SendBuffer.GetNumBits(), SendBuffer.GetNumBytes(), SendBuffer.GetMaxBits() );
	}
}

void UNetConnection::InitSendBuffer()
{
	check(MaxPacket > 0);

	int32 FinalBufferSize = (MaxPacket * 8) - MaxPacketHandlerBits;

	// Initialize the one outgoing buffer.
	if (FinalBufferSize == SendBuffer.GetMaxBits())
	{
		// Reset all of our values to their initial state without a malloc/free
		SendBuffer.Reset();
	}
	else
	{
		// First time initialization needs to allocate the buffer
		SendBuffer = FBitWriter(FinalBufferSize);
	}

	ResetPacketBitCounts();

	ValidateSendBuffer();
}

void UNetConnection::ReceivedRawPacket( void* InData, int32 Count )
{
#if !UE_BUILD_SHIPPING
	// Add an opportunity for the hook to block further processing
	bool bBlockReceive = false;

	ReceivedRawPacketDel.ExecuteIfBound(InData, Count, bBlockReceive);

	if (bBlockReceive)
	{
		return;
	}
#endif

	// Opportunity for packet loss burst simulation to drop the incoming packet.
	if (Driver && Driver->IsSimulatingPacketLossBurst())
	{
		return;
	}

	uint8* Data = (uint8*)InData;

	if (Handler.IsValid())
	{
		const ProcessedPacket UnProcessedPacket = Handler->Incoming(Data, Count);

		if (!UnProcessedPacket.bError)
		{
			Count = FMath::DivideAndRoundUp(UnProcessedPacket.CountBits, 8);

			if (Count > 0)
			{
				Data = UnProcessedPacket.Data;
			}
			// This packed has been consumed
			else
			{
				return;
			}
		}
		else
		{
			CLOSE_CONNECTION_DUE_TO_SECURITY_VIOLATION(this, ESecurityEvent::Malformed_Packet,
														TEXT("Packet failed PacketHandler processing."));

			return;
		}
		
		// See if we receive a packet that wasn't fully consumed by the handler before the handler is initialized.
		if (!Handler->IsFullyInitialized())
		{
			UE_LOG(LogNet, Warning, TEXT("PacketHander isn't fully initialized and also didn't fully consume a packet! This will cause the connection to try to send a packet before the initial packet sequence has been established. Ignoring. Connection: %s"), *Describe());
			return;
		}
	}


	// Handle an incoming raw packet from the driver.
	UE_LOG(LogNetTraffic, Verbose, TEXT("%6.3f: Received %i"), FPlatformTime::Seconds() - GStartTime, Count );
	int32 PacketBytes = Count + PacketOverhead;
	InBytes += PacketBytes;
	InTotalBytes += PacketBytes;
	++InPackets;
	++InTotalPackets;

	if (Driver)
	{
		Driver->InBytes += PacketBytes;
		Driver->InTotalBytes += PacketBytes;
		Driver->InPackets++;
		Driver->InTotalPackets++;
	}

	if (Count > 0)
	{
		uint8 LastByte = Data[Count-1];

		if (LastByte != 0)
		{
			int32 BitSize = (Count * 8) - 1;

			// Bit streaming, starts at the Least Significant Bit, and ends at the MSB.
			while (!(LastByte & 0x80))
			{
				LastByte *= 2;
				BitSize--;
			}


			FBitReader Reader(Data, BitSize);

			// Set the network version on the reader
			Reader.SetEngineNetVer( EngineNetworkProtocolVersion );
			Reader.SetGameNetVer( GameNetworkProtocolVersion );

			if (Handler.IsValid())
			{
				Handler->IncomingHigh(Reader);
			}

			if (Reader.GetBitsLeft() > 0)
			{
				ReceivedPacket(Reader);

				// Check if the out of order packet cache needs flushing
				FlushPacketOrderCache();
			}
		}
		// MalformedPacket - Received a packet with 0's in the last byte
		else
		{
			CLOSE_CONNECTION_DUE_TO_SECURITY_VIOLATION(this, ESecurityEvent::Malformed_Packet, TEXT("Received packet with 0's in last byte of packet"));
		}
	}
	// MalformedPacket - Received a packet of 0 bytes
	else 
	{
		CLOSE_CONNECTION_DUE_TO_SECURITY_VIOLATION(this, ESecurityEvent::Malformed_Packet, TEXT("Received zero-size packet"));
	}
}

void UNetConnection::FlushPacketOrderCache(bool bFlushWholeCache/*=false*/)
{
	if (PacketOrderCache.IsSet() && PacketOrderCacheCount > 0)
	{
		TCircularBuffer<TUniquePtr<FBitReader>>& Cache = PacketOrderCache.GetValue();
		int32 CacheEndIdx = PacketOrderCache->GetPreviousIndex(PacketOrderCacheStartIdx);
		bool bEndOfCacheSet = Cache[CacheEndIdx].IsValid();

		bFlushingPacketOrderCache = true;

		// If the end of the cache has had its value set, this forces the flushing of the whole cache, no matter how many missing sequences there are.
		// The reason for this (other than making space in the cache), is that when we receive a sequence that is out of range of the cache,
		// it is stored at the end, and so the cache index no longer lines up with the sequence number - which it needs to.
		bFlushWholeCache = bFlushWholeCache || bEndOfCacheSet;

		while (PacketOrderCacheCount > 0)
		{
			TUniquePtr<FBitReader>& CurCachePacket = Cache[PacketOrderCacheStartIdx];

			if (CurCachePacket.IsValid())
			{
				UE_LOG(LogNet, VeryVerbose, TEXT("'Out of Order' Packet Cache, replaying packet with cache index: %i (bFlushWholeCache: %i)"), PacketOrderCacheStartIdx, (int32)bFlushWholeCache);

				ReceivedPacket(*CurCachePacket.Get());

				CurCachePacket.Reset();

				PacketOrderCacheCount--;
			}
			// Advance the cache only up to the first missing packet, unless flushing the whole cache
			else if (!bFlushWholeCache)
			{
				break;
			}

			PacketOrderCacheStartIdx = PacketOrderCache->GetNextIndex(PacketOrderCacheStartIdx);
		}

		bFlushingPacketOrderCache = false;
	}
}

uint32 GNetOutBytes = 0;

void UNetConnection::FlushNet(bool bIgnoreSimulation)
{
	check(Driver);

	// Update info.
	ValidateSendBuffer();
	LastEnd = FBitWriterMark();
	TimeSensitive = 0;

	// If there is any pending data to send, send it.
	if (SendBuffer.GetNumBits() || HasDirtyAcks || ( Driver->Time-LastSendTime > Driver->KeepAliveTime && !InternalAck && State != USOCK_Closed))
	{
		// Due to the PacketHandler handshake code, servers must never send the client data,
		// before first receiving a client control packet (which is taken as an indication of a complete handshake).
		if (!HasReceivedClientPacket() && CVarRandomizeSequence.GetValueOnAnyThread() != 0)
		{
			UE_LOG(LogNet, Log, TEXT("Attempting to send data before handshake is complete. %s"), *Describe());
			Close();
			InitSendBuffer();
			return;
		}


		FOutPacketTraits Traits;

		// If sending keepalive packet or just acks, still write the packet header
		if (SendBuffer.GetNumBits() == 0)
		{
			WriteBitsToSendBuffer( NULL, 0 );		// This will force the packet header to be written

			Traits.bIsKeepAlive = true;
			AnalyticsVars.OutKeepAliveCount++;
		}


		// @todo #JohnB: Since OutgoingHigh uses SendBuffer, its ReservedPacketBits needs to be modified to account for this differently
		if (Handler.IsValid())
		{
			Handler->OutgoingHigh(SendBuffer);
		}


		// Write the UNetConnection-level termination bit
		SendBuffer.WriteBit(1);

		// Refresh outgoing header with latest data
		if ( !InternalAck )
		{
			// if we update ack, we also update received ack associated with outgoing seq
			// so we know how many ack bits we need to write (which is updated in received packet)
			WritePacketHeader(SendBuffer);
		}

		ValidateSendBuffer();

		const int32 NumStrayBits = SendBuffer.GetNumBits();

		// @todo: This is no longer accurate, given potential for PacketHandler termination bit and bit padding
		//NumPaddingBits += (NumStrayBits != 0) ? (8 - NumStrayBits) : 0;

		Traits.NumAckBits = NumAckBits;
		Traits.NumBunchBits = NumBunchBits;


		NETWORK_PROFILER(GNetworkProfiler.FlushOutgoingBunches(this));

		// Send now.
#if DO_ENABLE_NET_TEST
		// if the connection is closing/being destroyed/etc we need to send immediately regardless of settings
		// because we won't be around to send it delayed
		if (State == USOCK_Closed || IsGarbageCollecting() || bIgnoreSimulation || InternalAck)
		{
			// Checked in FlushNet() so each child class doesn't have to implement this
			if (Driver->IsNetResourceValid())
			{
				LowLevelSend(SendBuffer.GetData(), SendBuffer.GetNumBits(), Traits);
			}
		}
		else if (PacketSimulationSettings.PktOrder)
		{
			DelayedPacket& B = *(new(Delayed)DelayedPacket(SendBuffer.GetData(), SendBuffer.GetNumBits(), Traits));

			for (int32 i=Delayed.Num() - 1; i >= 0; i--)
			{
				if (FMath::FRand() > 0.50)
				{
					if (!ShouldDropOutgoingPacketForLossSimulation(SendBuffer.GetNumBits()))
					{
						// Checked in FlushNet() so each child class doesn't have to implement this
						if (Driver->IsNetResourceValid())
						{
							LowLevelSend((char*) &Delayed[i].Data[0], Delayed[i].SizeBits, Delayed[i].Traits);
						}
					}
					Delayed.RemoveAt(i);
				}
			}
		}
		else if (PacketSimulationSettings.PktLag)
		{
			if (!ShouldDropOutgoingPacketForLossSimulation(SendBuffer.GetNumBits()))
			{
				DelayedPacket& B = *(new(Delayed)DelayedPacket(SendBuffer.GetData(), SendBuffer.GetNumBits(), Traits));

				B.SendTime = FPlatformTime::Seconds() + (double(PacketSimulationSettings.PktLag)  + 2.0f * (FMath::FRand() - 0.5f) * double(PacketSimulationSettings.PktLagVariance))/ 1000.f;
			}
		}
		else if (!ShouldDropOutgoingPacketForLossSimulation(SendBuffer.GetNumBits()))
		{
#endif
			// Checked in FlushNet() so each child class doesn't have to implement this
			if (Driver->IsNetResourceValid())
			{
				LowLevelSend(SendBuffer.GetData(), SendBuffer.GetNumBits(), Traits);
			}
#if DO_ENABLE_NET_TEST
			if (PacketSimulationSettings.PktDup && FMath::FRand() * 100.f < PacketSimulationSettings.PktDup)
			{
				// Checked in FlushNet() so each child class doesn't have to implement this
				if (Driver->IsNetResourceValid())
				{
					LowLevelSend((char*) SendBuffer.GetData(), SendBuffer.GetNumBits(), Traits);
				}
			}
		}
#endif
		// Update stuff.
		const int32 Index = OutPacketId & (ARRAY_COUNT(OutLagPacketId)-1);

		// Remember the actual time this packet was sent out, so we can compute ping when the ack comes back
		OutLagPacketId[Index]			= OutPacketId;
		OutLagTime[Index]				= FPlatformTime::Seconds();
	
		OutBytesPerSecondHistory[Index]	= FMath::Min(OutBytesPerSecond / 1024, 255);

		// Increase outgoing sequence number
		if (!InternalAck)
		{
			PacketNotify.CommitAndIncrementOutSeq();
		}

		// Make sure that we always push an ChannelRecordEntry for each transmitted packet even if it is empty
		FChannelRecordImpl::PushPacketId(ChannelRecord, OutPacketId);

		++OutPacketId; 

		++OutPackets;
		++OutTotalPackets;
		Driver->OutPackets++;
		Driver->OutTotalPackets++;

		//Record the packet time to the histogram
		double LastPacketTimeDiffInMs = (Driver->Time - LastSendTime) * 1000.0;
		NetConnectionHistogram.AddMeasurement(LastPacketTimeDiffInMs);

		LastSendTime = Driver->Time;

		const int32 PacketBytes = SendBuffer.GetNumBytes() + PacketOverhead;

		QueuedBits += (PacketBytes * 8);

		OutBytes += PacketBytes;
		OutTotalBytes += PacketBytes;
		Driver->OutBytes += PacketBytes;
		Driver->OutTotalBytes += PacketBytes;
		GNetOutBytes += PacketBytes;

		AnalyticsVars.OutAckOnlyCount += (NumAckBits > 0 && NumBunchBits == 0);

		InitSendBuffer();
	}
}

bool UNetConnection::ShouldDropOutgoingPacketForLossSimulation(int64 NumBits) const
{
#if DO_ENABLE_NET_TEST
	return Driver->IsSimulatingPacketLossBurst() ||
		(NumBits > PacketSimulationSettings.PktLossMinSize * 8 && NumBits < PacketSimulationSettings.PktLossMaxSize * 8
		&& PacketSimulationSettings.PktLoss > 0 && FMath::FRand() * 100.f < PacketSimulationSettings.PktLoss);
#else
	return false;
#endif
}

int32 UNetConnection::IsNetReady( bool Saturate )
{
	// Return whether we can send more data without saturation the connection.
	if (Saturate)
	{
		QueuedBits = -SendBuffer.GetNumBits();
	}

	return QueuedBits + SendBuffer.GetNumBits() <= 0;
}

void UNetConnection::ReadInput( float DeltaSeconds )
{}

void UNetConnection::ReceivedAck(int32 AckPacketId)
{
	UE_LOG(LogNetTraffic, Verbose, TEXT("   Received ack %i"), AckPacketId);

	SCOPE_CYCLE_COUNTER(Stat_NetConnectionReceivedAcks);
	
	// Advance OutAckPacketId
	OutAckPacketId = AckPacketId;

	// Process the bunch.
	LastRecvAckTime = Driver->Time;

	if (PackageMap != NULL)
	{
		PackageMap->ReceivedAck( AckPacketId );
	}

	auto AckChannelFunc = [this](int32 AckedPacketId, uint32 ChannelIndex)
	{
		UChannel* const Channel = Channels[ChannelIndex];

		if (Channel)
		{
			if (Channel->OpenPacketId.Last == AckedPacketId) // Necessary for unreliable "bNetTemporary" channels.
			{
				Channel->OpenAcked = 1;
			}
				
			for (FOutBunch* OutBunch = Channel->OutRec; OutBunch; OutBunch = OutBunch->Next)
			{
				if (OutBunch->bOpen)
				{
					UE_LOG(LogNet, VeryVerbose, TEXT("Channel %i reset Ackd because open is reliable. "), Channel->ChIndex );
					Channel->OpenAcked  = 0; // We have a reliable open bunch, don't let the above code set the OpenAcked state,
											// it must be set in UChannel::ReceivedAcks to verify all open bunches were received.
				}

				if (OutBunch->PacketId == AckedPacketId)
				{
					OutBunch->ReceivedAck = 1;
				}
			}
			Channel->ReceivedAcks(); //warning: May destroy Channel.
		}
	};

	// Invoke AckChannelFunc on all channels written for this PacketId
	FChannelRecordImpl::ConsumeChannelRecordsForPacket(ChannelRecord, AckPacketId, AckChannelFunc);
}

void UNetConnection::ReceivedNak( int32 NakPacketId )
{
	UE_LOG(LogNetTraffic, Verbose, TEXT("   Received nak %i"), NakPacketId);

	SCOPE_CYCLE_COUNTER(Stat_NetConnectionReceivedNak);

	// Update pending NetGUIDs
	PackageMap->ReceivedNak(NakPacketId);

	auto NakChannelFunc = [this](int32 NackedPacketId, uint32 ChannelIndex)
	{
		UChannel* const Channel = Channels[ChannelIndex];
		if (Channel)
		{
			Channel->ReceivedNak(NackedPacketId);
			if (Channel->OpenPacketId.InRange(NackedPacketId))
			{
				Channel->ReceivedAcks(); //warning: May destroy Channel.
			}
		}
	};

	// Invoke NakChannelFunc on all channels written for this PacketId
	FChannelRecordImpl::ConsumeChannelRecordsForPacket(ChannelRecord, NakPacketId, NakChannelFunc);

	// Stats
	++OutPacketsLost;
	++OutTotalPacketsLost;
	++Driver->OutTotalPacketsLost;
}

// IMPORTANT:
// WritePacketHeader must ALWAYS write the exact same number of bits as we go back and rewrite the header
// right before we put the packet on the wire.
void UNetConnection::WritePacketHeader(FBitWriter& Writer)
{
	// If this is a header refresh, we only serialize the updated serial number information
	const bool bIsHeaderUpdate = Writer.GetNumBits() > 0u;

	// Header is always written first in the packet
	FBitWriterMark Reset;
	FBitWriterMark Restore(Writer);
	Reset.PopWithoutClear(Writer);
	
	// Write notification header or refresh the header if used space is the same.
	bool bWroteHeader = PacketNotify.WriteHeader(Writer, bIsHeaderUpdate);

	// Jump back to where we came from.
	if (bIsHeaderUpdate)
	{
		Restore.PopWithoutClear(Writer);

		// if we wrote the header and successfully refreshed the header status we no longer has any dirty acks
		if (bWroteHeader)
		{
			HasDirtyAcks = 0u;
		}
	}
}

void UNetConnection::WritePacketInfo(FBitWriter& Writer) const
{
	const uint8 bHasServerFrameTime = Driver->IsServer() ? bLastHasServerFrameTime : ( CVarPingExcludeFrameTime.GetValueOnGameThread() > 0 ? 1u : 0u );

	// Write data used to calculate link latency
	Writer.WriteBit(bHasServerFrameTime);
	if (bHasServerFrameTime && Driver->IsServer())
	{
		uint8 FrameTimeByte = FMath::Min( FMath::FloorToInt( FrameTime * 1000 ), 255 );
		Writer << FrameTimeByte;
	}

	// Notify server of our current rate per second at this time
	uint8 InKBytesPerSecondByte = FMath::Min(InBytesPerSecond / 1024, 255);
	Writer << InKBytesPerSecondByte;
}

bool UNetConnection::ReadPacketInfo(FBitReader& Reader)
{
	const bool bHasServerFrameTime = Reader.ReadBit() == 1u;
	double ServerFrameTime = 0;

	if ( !Driver->IsServer() )
	{
		if ( bHasServerFrameTime )
		{
			uint8 FrameTimeByte	= 0;
			Reader << FrameTimeByte;
			// As a client, our request was granted, read the frame time
			ServerFrameTime = ( double )FrameTimeByte / 1000;
		}
	}
	else
	{
		bLastHasServerFrameTime = bHasServerFrameTime;
	}

	// limit to known size to know the size of the packet header
	uint8 RemoteInKBytesPerSecondByte = 0;
	Reader << RemoteInKBytesPerSecondByte;

	if ( Reader.IsError() )
	{
		return false;
	}

	// Update ping
	// At this time we have updated OutAckPacketId to the latest received ack.
	const int32 Index = OutAckPacketId & (ARRAY_COUNT(OutLagPacketId)-1);

	if ( OutLagPacketId[Index] == OutAckPacketId )
	{
		OutLagPacketId[Index] = -1;		// Only use the ack once

#if !UE_BUILD_SHIPPING
		if ( CVarPingDisplayServerTime.GetValueOnAnyThread() > 0 )
		{
			UE_LOG( LogNetTraffic, Warning, TEXT( "ServerFrameTime: %2.2f" ), ServerFrameTime * 1000.0f );
		}
#endif

		// use FApp's time because it is set closer to the beginning of the frame - we don't care about the time so far of the current frame to process the packet
		const double CurrentTime = FApp::GetCurrentTime();
		const double GameTime	 = ServerFrameTime;
		const double RTT		 = (CurrentTime - OutLagTime[Index] ) - ( CVarPingExcludeFrameTime.GetValueOnAnyThread() ? GameTime : 0.0 );
		const double NewLag		 = FMath::Max( RTT, 0.0 );

		if ( OutBytesPerSecondHistory[Index] > 0 )
		{
			RemoteSaturation = ( 1.0f - FMath::Min( ( float )RemoteInKBytesPerSecondByte / ( float )OutBytesPerSecondHistory[Index], 1.0f ) ) * 100.0f;
		}
		else
		{
			RemoteSaturation = 0.0f;
		}

		//UE_LOG( LogNet, Warning, TEXT( "Out: %i, InRemote: %i, Saturation: %f" ), OutBytesPerSecondHistory[Index], RemoteInKBytesPerSecond, RemoteSaturation );

		LagAcc += NewLag;
		LagCount++;

		if (PlayerController != NULL)
		{
			PlayerController->UpdatePing(NewLag);
		}
	}

	return true;
}

FNetworkGUID UNetConnection::GetActorGUIDFromOpenBunch(FInBunch& Bunch)
{
	// NOTE: This could break if this is a PartialBunch and the ActorGUID wasn't serialized.
	//			Seems unlikely given the aggressive Flushing + increased MTU on InternalAck.

	// Any GUIDs / Exports will have been read already for InternalAck connections,
	// but we may have to skip over must-be-mapped GUIDs before we can read the actor GUID.

	if (Bunch.bHasMustBeMappedGUIDs)
	{
		uint16 NumMustBeMappedGUIDs = 0;
		Bunch << NumMustBeMappedGUIDs;

		for (int32 i = 0; i < NumMustBeMappedGUIDs; i++)
		{
			FNetworkGUID NetGUID;
			Bunch << NetGUID;
		}
	}

	NET_CHECKSUM( Bunch );

	FNetworkGUID ActorGUID;
	Bunch << ActorGUID;

	return ActorGUID;
}

void UNetConnection::ReceivedPacket( FBitReader& Reader )
{
	SCOPED_NAMED_EVENT(UNetConnection_ReceivedPacket, FColor::Green);
	AssertValid();

	// Handle PacketId.
	if( Reader.IsError() )
	{
		ensureMsgf(false, TEXT("Packet too small") );
		return;
	}


	FBitReaderMark ResetReaderMark(Reader);

	ValidateSendBuffer();

	//Record the packet time to the histogram
	double LastPacketTimeDiffInMs = (FPlatformTime::Seconds() - LastReceiveRealtime) * 1000.0;
	NetConnectionHistogram.AddMeasurement(LastPacketTimeDiffInMs);

	// Update receive time to avoid timeout.
	LastReceiveTime		= Driver->Time;
	LastReceiveRealtime = FPlatformTime::Seconds();

	if (InternalAck)
	{
		++InPacketId;
	}	
	else
	{
		// Read packet header
		FNetPacketNotify::FNotificationHeader Header;
		if (!PacketNotify.ReadHeader(Header, Reader))
		{
			CLOSE_CONNECTION_DUE_TO_SECURITY_VIOLATION(this, ESecurityEvent::Malformed_Packet, TEXT("Failed to read PacketHeader"));
			return;
		}


		bool bPacketOrderCacheActive = !bFlushingPacketOrderCache && PacketOrderCache.IsSet();
		bool bCheckForMissingSequence = bPacketOrderCacheActive && PacketOrderCacheCount == 0;
		bool bFillingPacketOrderCache = bPacketOrderCacheActive && PacketOrderCacheCount > 0;
		int32 MaxMissingPackets = (bCheckForMissingSequence ? CVarNetPacketOrderMaxMissingPackets.GetValueOnAnyThread() : 0);
		int32 PacketSequenceDelta = PacketNotify.GetSequenceDelta(Header);

		if (PacketSequenceDelta > 0)
		{
			const int32 MissingPacketCount = PacketSequenceDelta - 1;

			// Cache the packet if we are already caching, and begin caching if we just encountered a missing sequence, within range
			if (bFillingPacketOrderCache || (bCheckForMissingSequence && MissingPacketCount > 0 && MissingPacketCount <= MaxMissingPackets))
			{
				int32 LinearCacheIdx = PacketSequenceDelta - 1;
				int32 CacheCapacity = PacketOrderCache->Capacity();
				bool bLastCacheEntry = LinearCacheIdx >= (CacheCapacity - 1);

				// The last cache entry is only set, when we've reached capacity or when we receive a sequence which is out of bounds of the cache
				LinearCacheIdx = bLastCacheEntry ? (CacheCapacity - 1) : LinearCacheIdx;

				int32 CiruclarCacheIdx = PacketOrderCacheStartIdx;

				for (int32 LinearDec=LinearCacheIdx; LinearDec > 0; LinearDec--)
				{
					CiruclarCacheIdx = PacketOrderCache->GetNextIndex(CiruclarCacheIdx);
				}

				TUniquePtr<FBitReader>& CurCachePacket = PacketOrderCache.GetValue()[CiruclarCacheIdx];

				// Reset the reader to its initial position, and cache the packet
				if (!CurCachePacket.IsValid())
				{
					UE_LOG(LogNet, VeryVerbose, TEXT("'Out of Order' Packet Cache, caching sequence order '%i' (capacity: %i)"), LinearCacheIdx, CacheCapacity);

					CurCachePacket = MakeUnique<FBitReader>(Reader);
					PacketOrderCacheCount++;

					ResetReaderMark.Pop(*CurCachePacket);
				}
				else
				{
					TotalOutOfOrderPackets++;
					Driver->InOutOfOrderPackets++;
				}

				return;
			}


			if (MissingPacketCount > 10)
			{
				UE_LOG(LogNetTraffic, Verbose, TEXT("High single frame packet loss. PacketsLost: %i %s" ), MissingPacketCount, *Describe());
			}

			InPacketsLost += MissingPacketCount;
			InTotalPacketsLost += MissingPacketCount;
			Driver->InPacketsLost += MissingPacketCount;
			Driver->InTotalPacketsLost += MissingPacketCount;
			InPacketId += PacketSequenceDelta;
		}
		else
		{
			TotalOutOfOrderPackets++;
			Driver->InOutOfOrderPackets++;

			if (!PacketOrderCache.IsSet() && CVarNetDoPacketOrderCorrection.GetValueOnAnyThread() != 0)
			{
				int32 EnableThreshold = CVarNetPacketOrderCorrectionEnableThreshold.GetValueOnAnyThread();

				if (TotalOutOfOrderPackets >= EnableThreshold)
				{
					UE_LOG(LogNet, Verbose, TEXT("Hit threshold of %i 'out of order' packet sequences. Enabling out of order packet correction."), EnableThreshold);

					int32 CacheSize = FMath::RoundUpToPowerOfTwo(CVarNetPacketOrderMaxCachedPackets.GetValueOnAnyThread());

					PacketOrderCache.Emplace(CacheSize);
				}
			}

			// Protect against replay attacks
			// We already protect against this for reliable bunches, and unreliable properties
			// The only bunch we would process would be unreliable RPC's, which could allow for replay attacks
			// So rather than add individual protection for unreliable RPC's as well, just kill it at the source, 
			// which protects everything in one fell swoop
			return;
		}


		// Lambda to dispatch delivery notifications, 
		auto HandlePacketNotification = [&Header, this](FNetPacketNotify::SequenceNumberT AckedSequence, bool bDelivered)
		{
			// Increase LastNotifiedPacketId, this is a full packet Id
			++LastNotifiedPacketId;
			++OutTotalNotifiedPackets;
			Driver->IncreaseOutTotalNotifiedPackets();

			// Sanity check
			if (FNetPacketNotify::SequenceNumberT(LastNotifiedPacketId) != AckedSequence)
			{
				CLOSE_CONNECTION_DUE_TO_SECURITY_VIOLATION(this, ESecurityEvent::Malformed_Packet, TEXT("LastNotifiedPacketId != AckedSequence"));
				return;
			}

			if (bDelivered)
			{
				ReceivedAck(LastNotifiedPacketId);
			}
			else
			{
				ReceivedNak(LastNotifiedPacketId);
			};
		};

		// Update incoming sequence data and deliver packet notifications
		// Packet is only accepted if both the incoming sequence number and incoming ack data are valid
		PacketNotify.Update(Header, HandlePacketNotification);

		// Extra information associated with the header (read only after acks have been processed)
		if (PacketSequenceDelta > 0 && !ReadPacketInfo(Reader))
		{
			CLOSE_CONNECTION_DUE_TO_SECURITY_VIOLATION(this, ESecurityEvent::Malformed_Packet, TEXT("Failed to read PacketHeader"));
			return;
		}
	}


	const bool bIgnoreRPCs = Driver->ShouldIgnoreRPCs();

	bool bSkipAck = false;

	// Track channels that were rejected while processing this packet - used to avoid sending multiple close-channel bunches,
	// which would cause a disconnect serverside
	TArray<int32> RejectedChans;

	// Disassemble and dispatch all bunches in the packet.
	while( !Reader.AtEnd() && State!=USOCK_Closed )
	{
		// For demo backwards compatibility, old replays still have this bit
		if (InternalAck && EngineNetworkProtocolVersion < EEngineNetworkVersionHistory::HISTORY_ACKS_INCLUDED_IN_HEADER)
		{
			const bool IsAckDummy = Reader.ReadBit() == 1u;
		}

		// Parse the bunch.
		int32 StartPos = Reader.GetPosBits();
		
		// Process Received data
		{
			// Parse the incoming data.
			FInBunch Bunch( this );
			int32 IncomingStartPos		= Reader.GetPosBits();
			uint8 bControl				= Reader.ReadBit();
			Bunch.PacketId				= InPacketId;
			Bunch.bOpen					= bControl ? Reader.ReadBit() : 0;
			Bunch.bClose				= bControl ? Reader.ReadBit() : 0;
			
PRAGMA_DISABLE_DEPRECATION_WARNINGS
			if (Bunch.EngineNetVer() < HISTORY_CHANNEL_CLOSE_REASON)
			{
				Bunch.bDormant = Bunch.bClose ? Reader.ReadBit() : 0;
				Bunch.CloseReason = Bunch.bDormant ? EChannelCloseReason::Dormancy : EChannelCloseReason::Destroyed;
			}
			else
			{
				Bunch.CloseReason = Bunch.bClose ? (EChannelCloseReason)Reader.ReadInt((uint32)EChannelCloseReason::MAX) : EChannelCloseReason::Destroyed;
				Bunch.bDormant = (Bunch.CloseReason == EChannelCloseReason::Dormancy);
			}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

			Bunch.bIsReplicationPaused  = Reader.ReadBit();
			Bunch.bReliable				= Reader.ReadBit();

			if (Bunch.EngineNetVer() < HISTORY_MAX_ACTOR_CHANNELS_CUSTOMIZATION)
			{
				static const int OLD_MAX_ACTOR_CHANNELS = 10240;
				Bunch.ChIndex = Reader.ReadInt(OLD_MAX_ACTOR_CHANNELS);
			}
			else
			{
				uint32 ChIndex;
				Reader.SerializeIntPacked(ChIndex);

				if (ChIndex >= (uint32)MaxChannelSize)
				{
					CLOSE_CONNECTION_DUE_TO_SECURITY_VIOLATION(this, ESecurityEvent::Malformed_Packet, TEXT("Bunch channel index exceeds channel limit"));
					return;
				}

				Bunch.ChIndex = ChIndex;
			}

			Bunch.bHasPackageMapExports	= Reader.ReadBit();
			Bunch.bHasMustBeMappedGUIDs	= Reader.ReadBit();
			Bunch.bPartial				= Reader.ReadBit();

			if ( Bunch.bReliable )
			{
				if ( InternalAck )
				{
					// We can derive the sequence for 100% reliable connections
					Bunch.ChSequence = InReliable[Bunch.ChIndex] + 1;
				}
				else
				{
					// If this is a reliable bunch, use the last processed reliable sequence to read the new reliable sequence
					Bunch.ChSequence = MakeRelative( Reader.ReadInt( MAX_CHSEQUENCE ), InReliable[Bunch.ChIndex], MAX_CHSEQUENCE );
				}
			} 
			else if ( Bunch.bPartial )
			{
				// If this is an unreliable partial bunch, we simply use packet sequence since we already have it
				Bunch.ChSequence = InPacketId;
			}
			else
			{
				Bunch.ChSequence = 0;
			}

			Bunch.bPartialInitial = Bunch.bPartial ? Reader.ReadBit() : 0;
			Bunch.bPartialFinal = Bunch.bPartial ? Reader.ReadBit() : 0;

			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			if (Bunch.EngineNetVer() < HISTORY_CHANNEL_NAMES)
			{
				Bunch.ChType = (Bunch.bReliable || Bunch.bOpen) ? Reader.ReadInt(CHTYPE_MAX) : CHTYPE_None;
				switch (Bunch.ChType)
				{
					case CHTYPE_Control:
						Bunch.ChName = NAME_Control;
						break;
					case CHTYPE_Voice:
						Bunch.ChName = NAME_Voice;
						break;
					case CHTYPE_Actor:
						Bunch.ChName = NAME_Actor;
						break;
					break;
				}
			}
			else
			{
				if (Bunch.bReliable || Bunch.bOpen)
				{
					UPackageMap::StaticSerializeName(Reader, Bunch.ChName);

					if( Reader.IsError() )
					{
						CLOSE_CONNECTION_DUE_TO_SECURITY_VIOLATION(this, ESecurityEvent::Malformed_Packet, TEXT("Channel name serialization failed."));
						return;
					}

					if (Bunch.ChName == NAME_Control)
					{
						Bunch.ChType = CHTYPE_Control;
					}
					else if (Bunch.ChName == NAME_Voice)
					{
						Bunch.ChType = CHTYPE_Voice;
					}
					else if (Bunch.ChName == NAME_Actor)
					{
						Bunch.ChType = CHTYPE_Actor;
					}
				}
				else
				{
					Bunch.ChType = CHTYPE_None;
					Bunch.ChName = NAME_None;
				}
			}
			PRAGMA_ENABLE_DEPRECATION_WARNINGS

			UChannel* Channel = Channels[Bunch.ChIndex];

			// If there's an existing channel and the bunch specified it's channel type, make sure they match.
			if (Channel && (Bunch.ChName != NAME_None) && (Bunch.ChName != Channel->ChName))
			{
				UE_LOG(LogNet, Error, TEXT("Existing channel at index %d with type \"%s\" differs from the incoming bunch's expected channel type, \"%s\"."),
					Bunch.ChIndex, *Channel->ChName.ToString(), *Bunch.ChName.ToString());
				Close();
				return;
			}

			int32 BunchDataBits  = Reader.ReadInt( UNetConnection::MaxPacket*8 );

			if ((Bunch.bClose || Bunch.bOpen) && UE_LOG_ACTIVE(LogNetDormancy,VeryVerbose) )
			{
				UE_LOG(LogNetDormancy, VeryVerbose, TEXT("Received: %s"), *Bunch.ToString());
			}

			if (UE_LOG_ACTIVE(LogNetTraffic,VeryVerbose))
			{
				UE_LOG(LogNetTraffic, VeryVerbose, TEXT("Received: %s"), *Bunch.ToString());
			}

			const int32 HeaderPos = Reader.GetPosBits();

			if( Reader.IsError() )
			{
				CLOSE_CONNECTION_DUE_TO_SECURITY_VIOLATION(this, ESecurityEvent::Malformed_Packet, TEXT("Bunch header overflowed"));
				return;
			}
			Bunch.SetData( Reader, BunchDataBits );
			if( Reader.IsError() )
			{
				// Bunch claims it's larger than the enclosing packet.
				CLOSE_CONNECTION_DUE_TO_SECURITY_VIOLATION(this, ESecurityEvent::Invalid_Data, TEXT("Bunch data overflowed (%i %i+%i/%i)"), IncomingStartPos, HeaderPos, BunchDataBits, Reader.GetNumBits());
				return;
			}

			if ( Bunch.bHasPackageMapExports )
			{
				Driver->NetGUIDInBytes += (BunchDataBits + (HeaderPos - IncomingStartPos)) >> 3;

				if ( InternalAck )
				{
					// NOTE - For replays, we do this even earlier, to try and load this as soon as possible, in case there is an issue creating the channel
					// If a replay fails to create a channel, we want to salvage as much as possible
					Cast<UPackageMapClient>( PackageMap )->ReceiveNetGUIDBunch( Bunch );

					if ( Bunch.IsError() )
					{
						UE_LOG( LogNetTraffic, Error, TEXT( "UNetConnection::ReceivedPacket: Bunch.IsError() after ReceiveNetGUIDBunch. ChIndex: %i" ), Bunch.ChIndex );
					}
				}
			}

			if( Bunch.bReliable )
			{
				UE_LOG(LogNetTraffic, Verbose, TEXT("   Reliable Bunch, Channel %i Sequence %i: Size %.1f+%.1f"), Bunch.ChIndex, Bunch.ChSequence, (HeaderPos-IncomingStartPos)/8.f, (Reader.GetPosBits()-HeaderPos)/8.f );
			}
			else
			{
				UE_LOG(LogNetTraffic, Verbose, TEXT("   Unreliable Bunch, Channel %i: Size %.1f+%.1f"), Bunch.ChIndex, (HeaderPos-IncomingStartPos)/8.f, (Reader.GetPosBits()-HeaderPos)/8.f );
			}

			if ( Bunch.bOpen )
			{
				UE_LOG(LogNetTraffic, Verbose, TEXT("   bOpen Bunch, Channel %i Sequence %i: Size %.1f+%.1f"), Bunch.ChIndex, Bunch.ChSequence, (HeaderPos-IncomingStartPos)/8.f, (Reader.GetPosBits()-HeaderPos)/8.f );
			}

			if ( Channels[Bunch.ChIndex] == NULL && ( Bunch.ChIndex != 0 || Bunch.ChName != NAME_Control ) )
			{
				// Can't handle other channels until control channel exists.
				if ( Channels[0] == NULL )
				{
					//CLOSE_CONNECTION_DUE_TO_SECURITY_VIOLATION(this, ESecurityEvent::Malformed_Packet, TEXT( "UNetConnection::ReceivedPacket: Received non-control bunch before control channel was created. ChIndex: %i, ChType: %i" ), Bunch.ChIndex, Bunch.ChType);
					UE_LOG( LogNetTraffic, Log, TEXT( "UNetConnection::ReceivedPacket: Received non-control bunch before control channel was created. ChIndex: %i, ChName: %s" ), Bunch.ChIndex, *Bunch.ChName.ToString() );
					Close();
					return;
				}
				// on the server, if we receive bunch data for a channel that doesn't exist while we're still logging in,
				// it's either a broken client or a new instance of a previous connection,
				// so reject it
				else if ( PlayerController == NULL && Driver->ClientConnections.Contains( this ) )
				{
					CLOSE_CONNECTION_DUE_TO_SECURITY_VIOLATION(this, ESecurityEvent::Malformed_Packet, TEXT( "UNetConnection::ReceivedPacket: Received non-control bunch before player controller was assigned. ChIndex: %i, ChName: %s" ), Bunch.ChIndex, *Bunch.ChName.ToString());
					return;
				}
			}
			// ignore control channel close if it hasn't been opened yet
			if ( Bunch.ChIndex == 0 && Channels[0] == NULL && Bunch.bClose && Bunch.ChName == NAME_Control )
			{
				//CLOSE_CONNECTION_DUE_TO_SECURITY_VIOLATION(this, ESecurityEvent::Malformed_Packet, TEXT( "UNetConnection::ReceivedPacket: Received control channel close before open" ));
				UE_LOG( LogNetTraffic, Log, TEXT( "UNetConnection::ReceivedPacket: Received control channel close before open" ) );
				Close();
				return;
			}

			// Receiving data.

			// We're on a 100% reliable connection and we are rolling back some data.
			// In that case, we can generally ignore these bunches.
			if (InternalAck && Channel && bIgnoreAlreadyOpenedChannels)
			{
				// This was an open bunch for a channel that's already opened.
				// We can ignore future bunches from this channel.
				const bool bNewlyOpenedActorChannel = Bunch.bOpen && (Bunch.ChName == NAME_Actor) && (!Bunch.bPartial || Bunch.bPartialInitial);

				if (bNewlyOpenedActorChannel)
				{
					FNetworkGUID ActorGUID = GetActorGUIDFromOpenBunch(Bunch);

					if (!Bunch.IsError())
					{
						IgnoringChannels.Add(Bunch.ChIndex, ActorGUID);
					}
					else
					{
						UE_LOG(LogNetTraffic, Error, TEXT("UNetConnection::ReceivedPacket: Unable to read actor GUID for ignored bunch. (Channel %d)"), Bunch.ChIndex);
					}
				}

				if (IgnoringChannels.Contains(Bunch.ChIndex))
				{
					if (Bunch.bClose && (!Bunch.bPartial || Bunch.bPartialFinal))
					{
						FNetworkGUID ActorGUID = IgnoringChannels.FindAndRemoveChecked(Bunch.ChIndex);
						if (ActorGUID.IsStatic())
						{
							UObject* FoundObject = Driver->GuidCache->GetObjectFromNetGUID(ActorGUID, false);
							if (AActor* StaticActor = Cast<AActor>(FoundObject))
							{
								DestroyIgnoredActor(StaticActor);
							}
							else
							{
								ensure(FoundObject == nullptr);
								UE_LOG(LogNetTraffic, Log, TEXT("UNetConnection::ReceivedPacket: Unable to find static actor to cleanup for ignored bunch. (Channel %d NetGUID %lu)"), Bunch.ChIndex, ActorGUID.Value);
							}
						}
					}

					UE_LOG(LogNetTraffic, Log, TEXT("Ignoring bunch for already open channel: %i"), Bunch.ChIndex);
					continue;
				}
			}

			// Ignore if reliable packet has already been processed.
			if ( Bunch.bReliable && Bunch.ChSequence <= InReliable[Bunch.ChIndex] )
			{
				UE_LOG( LogNetTraffic, Log, TEXT( "UNetConnection::ReceivedPacket: Received outdated bunch (Channel %d Current Sequence %i)" ), Bunch.ChIndex, InReliable[Bunch.ChIndex] );
				check( !InternalAck );		// Should be impossible with 100% reliable connections
				continue;
			}
			
			// If opening the channel with an unreliable packet, check that it is "bNetTemporary", otherwise discard it
			if( !Channel && !Bunch.bReliable )
			{
				// Unreliable bunches that open channels should be bOpen && (bClose || bPartial)
				// NetTemporary usually means one bunch that is unreliable (bOpen and bClose):	1(bOpen, bClose)
				// But if that bunch export NetGUIDs, it will get split into 2 bunches:			1(bOpen, bPartial) - 2(bClose).
				// (the initial actor bunch itself could also be split into multiple bunches. So bPartial is the right check here)

				const bool ValidUnreliableOpen = Bunch.bOpen && (Bunch.bClose || Bunch.bPartial);
				if (!ValidUnreliableOpen)
				{
					if ( InternalAck )
					{
						// Should be impossible with 100% reliable connections
						UE_LOG( LogNetTraffic, Error, TEXT( "      Received unreliable bunch before open with reliable connection (Channel %d Current Sequence %i)" ), Bunch.ChIndex, InReliable[Bunch.ChIndex] );
					}
					else
					{
						// Simply a log (not a warning, since this can happen under normal conditions, like from a re-join, etc)
						UE_LOG( LogNetTraffic, Log, TEXT( "      Received unreliable bunch before open (Channel %d Current Sequence %i)" ), Bunch.ChIndex, InReliable[Bunch.ChIndex] );
					}

					// Since we won't be processing this packet, don't ack it
					// We don't want the sender to think this bunch was processed when it really wasn't
					bSkipAck = true;
					continue;
				}
			}

			// Create channel if necessary.
			if (Channel == nullptr)
			{
				if (RejectedChans.Contains(Bunch.ChIndex))
				{
					UE_LOG(LogNetTraffic, Log, TEXT("      Ignoring Bunch for ChIndex %i, as the channel was already rejected while processing this packet."), Bunch.ChIndex);

					continue;
				}

				// Validate channel type.
				if ( !Driver->IsKnownChannelName( Bunch.ChName ) )
				{
					// Unknown type.
					CLOSE_CONNECTION_DUE_TO_SECURITY_VIOLATION(this, ESecurityEvent::Invalid_Data, TEXT( "UNetConnection::ReceivedPacket: Connection unknown channel type (%s)" ), *Bunch.ChName.ToString());
					return;
				}

				// Ignore incoming data on channel types that the client are not allowed to create. This can occur if we have in-flight data when server is closing a channel
				if ( Driver->IsServer() && (Driver->ChannelDefinitionMap[Bunch.ChName].bClientOpen == false) )
				{
					UE_LOG(LogNetTraffic, Warning, TEXT("      Ignoring Bunch Create received from client since only server is allowed to create this type of channel: Bunch  %i: ChName %s, ChSequence: %i, bReliable: %i, bPartial: %i, bPartialInitial: %i, bPartialFinal: %i"), Bunch.ChIndex, *Bunch.ChName.ToString(), Bunch.ChSequence, (int)Bunch.bReliable, (int)Bunch.bPartial, (int)Bunch.bPartialInitial, (int)Bunch.bPartialFinal );
					RejectedChans.AddUnique(Bunch.ChIndex);
					continue;
				}

				// peek for guid
				if (InternalAck && bIgnoreActorBunches)
				{
					if (Bunch.bOpen && (!Bunch.bPartial || Bunch.bPartialInitial) && (Bunch.ChName == NAME_Actor))
					{
						FBitReaderMark Mark(Bunch);
						FNetworkGUID ActorGUID = GetActorGUIDFromOpenBunch(Bunch);
						Mark.Pop(Bunch);

						if (ActorGUID.IsValid() && !ActorGUID.IsDefault())
						{
							if (IgnoredBunchGuids.Contains(ActorGUID))
							{
								UE_LOG(LogNetTraffic, Verbose, TEXT("Adding Channel: %i to ignore list, ignoring guid: %s"), Bunch.ChIndex, *ActorGUID.ToString());
								IgnoredBunchChannels.Add(Bunch.ChIndex);
								continue;
							}
							else
							{
								if (IgnoredBunchChannels.Remove(Bunch.ChIndex))
								{
									UE_LOG(LogNetTraffic, Verbose, TEXT("Removing Channel: %i from ignore list, got new guid: %s"), Bunch.ChIndex, *ActorGUID.ToString());
								}
							}
						}
						else
						{
							UE_LOG(LogNetTraffic, Warning, TEXT("Open bunch with invalid actor guid, Channel: %i"), Bunch.ChIndex);
						}
					}
					else
					{
						if (IgnoredBunchChannels.Contains(Bunch.ChIndex))
						{
							UE_LOG(LogNetTraffic, Verbose, TEXT("Ignoring bunch on channel: %i"), Bunch.ChIndex);
							continue;
						}
					}
				}

				// Reliable (either open or later), so create new channel.
				UE_LOG(LogNetTraffic, Log, TEXT("      Bunch Create %i: ChName %s, ChSequence: %i, bReliable: %i, bPartial: %i, bPartialInitial: %i, bPartialFinal: %i"), Bunch.ChIndex, *Bunch.ChName.ToString(), Bunch.ChSequence, (int)Bunch.bReliable, (int)Bunch.bPartial, (int)Bunch.bPartialInitial, (int)Bunch.bPartialFinal );
				Channel = CreateChannelByName( Bunch.ChName, EChannelCreateFlags::None, Bunch.ChIndex );

				// Notify the server of the new channel.
				if( !Driver->Notify->NotifyAcceptingChannel( Channel ) )
				{
					// Channel refused, so close it, flush it, and delete it.
					UE_LOG(LogNet, Verbose, TEXT("      NotifyAcceptingChannel Failed! Channel: %s"), *Channel->Describe() );

					RejectedChans.AddUnique(Bunch.ChIndex);

					FOutBunch CloseBunch( Channel, true );
					check(!CloseBunch.IsError());
					check(CloseBunch.bClose);
					CloseBunch.bReliable = 1;
					Channel->SendBunch( &CloseBunch, false );
					FlushNet();
					Channel->ConditionalCleanUp(false, EChannelCloseReason::Destroyed);
					if( Bunch.ChIndex==0 )
					{
						UE_LOG(LogNetTraffic, Log, TEXT("Channel 0 create failed") );
						State = USOCK_Closed;
					}
					continue;
				}
			}

			Bunch.bIgnoreRPCs = bIgnoreRPCs;

			// Dispatch the raw, unsequenced bunch to the channel.
			bool bLocalSkipAck = false;
			Channel->ReceivedRawBunch( Bunch, bLocalSkipAck ); //warning: May destroy channel.
			if ( bLocalSkipAck )
			{
				bSkipAck = true;
			}
			Driver->InBunches++;
			Driver->InTotalBunches++;

			// Disconnect if we received a corrupted packet from the client (eg server crash attempt).
			if ( !Driver->ServerConnection && ( Bunch.IsCriticalError() || Bunch.IsError() ) )
			{
				UE_LOG( LogNetTraffic, Error, TEXT("Received corrupted packet data from client %s.  Disconnecting."), *LowLevelGetRemoteAddress() );
				Close();
				bSkipAck = true;
			}
		}
	}

	ValidateSendBuffer();

	// Acknowledge the packet.
	if ( !bSkipAck )
	{
		LastGoodPacketRealtime = FPlatformTime::Seconds();
	}

	if( !InternalAck )
	{
		// We always call AckSequence even if we are explicitly rejecting the packet as this updates the expected InSeq used to drive future acks.
		if ( bSkipAck )
		{
			// Explicit Nak, we treat this packet as dropped but we still report it to the sending side as quickly as possible
			PacketNotify.NakSeq( InPacketId );
		}
		else
		{
			PacketNotify.AckSeq( InPacketId );

			// Keep stats happy
			++OutTotalAcks;
			++Driver->OutTotalAcks;
		}

		// We do want to let the other side know about the ack, so even if there are no other outgoing data when we tick the connection we will send an ackpacket.
		TimeSensitive = 1;
		++HasDirtyAcks;

		// This is to allow us to recover from hitches were we process more than FNetPacketNotify::SequenceHistoryLength packets in a row withouht sending out any packets.
		// In most cases this allows us to recover smoothly without reporting any pessimistic naks which will be the case if we overshoot the ack history.
		// Note: This should only occur if we are running with no timeouts!
		if (HasDirtyAcks >= FNetPacketNotify::MaxSequenceHistoryLength)
		{
			UE_LOG(LogNet, Warning, TEXT("UNetConnection::ReceivedPacket - Too many received packets to ack (%u) since last sent packet. InSeq: %u %s NextOutGoingSeq: %u"), HasDirtyAcks, PacketNotify.GetInSeq().Get(), *Describe(), PacketNotify.GetOutSeq().Get());

			FlushNet();
			if (HasDirtyAcks) // if acks still are dirty, flush again
			{
				FlushNet();
			}
		}
	}
}

void UNetConnection::SetIgnoreAlreadyOpenedChannels(bool bInIgnoreAlreadyOpenedChannels)
{
	check(InternalAck);
	bIgnoreAlreadyOpenedChannels = bInIgnoreAlreadyOpenedChannels;
	IgnoringChannels.Reset();
}

void UNetConnection::SetIgnoreActorBunches(bool bInIgnoreActorBunches, TSet<FNetworkGUID>&& InIgnoredBunchGuids)
{
	check(InternalAck);
	bIgnoreActorBunches = bInIgnoreActorBunches;

	IgnoredBunchChannels.Empty();
	InIgnoredBunchGuids.Empty();

	if (bIgnoreActorBunches)
	{
		IgnoredBunchGuids = MoveTemp(InIgnoredBunchGuids);
	}
}

int32 UNetConnection::WriteBitsToSendBuffer( 
	const uint8 *	Bits, 
	const int32		SizeInBits, 
	const uint8 *	ExtraBits, 
	const int32		ExtraSizeInBits,
	EWriteBitsDataType DataType)
{
	ValidateSendBuffer();

#if !UE_BUILD_SHIPPING
	// Now that the stateless handshake is responsible for initializing the packet sequence numbers,
	//	we can't allow any packets to be written to the send buffer until after this has completed
	if (CVarRandomizeSequence.GetValueOnAnyThread() > 0)
	{
		checkf(!Handler.IsValid() || Handler->IsFullyInitialized(), TEXT("Attempted to write to send buffer before packet handler was fully initialized. Connection: %s"), *Describe());
	}
#endif

	const int32 TotalSizeInBits = SizeInBits + ExtraSizeInBits;

	// Flush if we can't add to current buffer
	if ( TotalSizeInBits > GetFreeSendBufferBits() )
	{
		FlushNet();
	}

	// Remember start position in case we want to undo this write
	// Store this after the possible flush above so we have the correct start position in the case that we do flush
	LastStart = FBitWriterMark( SendBuffer );

	// If this is the start of the queue, make sure to add the packet id
	if ( SendBuffer.GetNumBits() == 0 && !InternalAck )
	{
		// Write Packet Header, before sending the packet we will go back and rewrite the data
		WritePacketHeader(SendBuffer);

		// Also write server RTT and Received rate
		WritePacketInfo(SendBuffer);

		// We do not allow the first bunch to merge with the ack data as this will "revert" the ack data.
		AllowMerge = false;
	
		// Update stats for PacketIdBits and ackdata (also including the data used for packet RTT and saturation calculations)
		int64 BitsWritten = SendBuffer.GetNumBits();
		NumPacketIdBits += FNetPacketNotify::SequenceNumberT::SeqNumberBits;
		NumAckBits += BitsWritten - FNetPacketNotify::SequenceNumberT::SeqNumberBits;

		// Report stats to profiler
		NETWORK_PROFILER( GNetworkProfiler.TrackSendAck( NumAckBits, this ) );

		ValidateSendBuffer();
	}

	// Add the bits to the queue
	if ( SizeInBits )
	{
		SendBuffer.SerializeBits( const_cast< uint8* >( Bits ), SizeInBits );
		ValidateSendBuffer();
	}

	// Add any extra bits
	if ( ExtraSizeInBits )
	{
		SendBuffer.SerializeBits( const_cast< uint8* >( ExtraBits ), ExtraSizeInBits );
		ValidateSendBuffer();
	}

	const int32 RememberedPacketId = OutPacketId;

	switch ( DataType )
	{
		case EWriteBitsDataType::Bunch:
			NumBunchBits += SizeInBits + ExtraSizeInBits;
			break;
		default:
			break;
	}

	// Flush now if we are full
	if (GetFreeSendBufferBits() == 0
#if !UE_BUILD_SHIPPING
		|| CVarForceNetFlush.GetValueOnAnyThread() != 0
#endif
		)
	{
		FlushNet();
	}

	return RememberedPacketId;
}

/** Returns number of bits left in current packet that can be used without causing a flush  */
int64 UNetConnection::GetFreeSendBufferBits()
{
	// If we haven't sent anything yet, make sure to account for the packet header + trailer size
	// Otherwise, we only need to account for trailer size
	const int32 ExtraBits = ( SendBuffer.GetNumBits() > 0 ) ? MAX_PACKET_TRAILER_BITS : MAX_PACKET_HEADER_BITS + MAX_PACKET_TRAILER_BITS;

	const int32 NumberOfFreeBits = SendBuffer.GetMaxBits() - ( SendBuffer.GetNumBits() + ExtraBits );

	check( NumberOfFreeBits >= 0 );

	return NumberOfFreeBits;
}

void UNetConnection::PopLastStart()
{
	NumBunchBits -= SendBuffer.GetNumBits() - LastStart.GetNumBits();
	LastStart.Pop(SendBuffer);
	NETWORK_PROFILER(GNetworkProfiler.PopSendBunch(this));
}

TSharedPtr<FObjectReplicator> UNetConnection::CreateReplicatorForNewActorChannel(UObject* Object)
{
	TSharedPtr<FObjectReplicator> NewReplicator = MakeShareable(new FObjectReplicator());
	NewReplicator->InitWithObject( Object, this, true );
	return NewReplicator;
}

void UNetConnection::PurgeAcks()
{
}

void UNetConnection::SendAck(int32 AckPacketId, bool FirstTime/*=1*/)
{
}

int32 UNetConnection::SendRawBunch( FOutBunch& Bunch, bool InAllowMerge )
{
	ValidateSendBuffer();
	check(!Bunch.ReceivedAck);
	check(!Bunch.IsError());
	Driver->OutBunches++;
	Driver->OutTotalBunches++;
	TimeSensitive = 1;

	// Build header.
	SendBunchHeader.Reset();

	SendBunchHeader.WriteBit( Bunch.bOpen || Bunch.bClose );
	if( Bunch.bOpen || Bunch.bClose )
	{
		SendBunchHeader.WriteBit( Bunch.bOpen );
		SendBunchHeader.WriteBit( Bunch.bClose );
		if( Bunch.bClose )
		{
			uint32 Value = (uint32)Bunch.CloseReason;
			SendBunchHeader.SerializeInt( Value, (uint32)EChannelCloseReason::MAX );
		}
	}
	SendBunchHeader.WriteBit( Bunch.bIsReplicationPaused );
	SendBunchHeader.WriteBit( Bunch.bReliable );

	uint32 ChIndex = Bunch.ChIndex;
	SendBunchHeader.SerializeIntPacked(ChIndex); 

	SendBunchHeader.WriteBit( Bunch.bHasPackageMapExports );
	SendBunchHeader.WriteBit( Bunch.bHasMustBeMappedGUIDs );
	SendBunchHeader.WriteBit( Bunch.bPartial );

	if ( Bunch.bReliable && !InternalAck )
	{
		SendBunchHeader.WriteIntWrapped(Bunch.ChSequence, MAX_CHSEQUENCE);
	}

	if (Bunch.bPartial)
	{
		SendBunchHeader.WriteBit( Bunch.bPartialInitial );
		SendBunchHeader.WriteBit( Bunch.bPartialFinal );
	}

	if (Bunch.bReliable || Bunch.bOpen)
	{
		UPackageMap::StaticSerializeName(SendBunchHeader, Bunch.ChName);
	}
	
	SendBunchHeader.WriteIntWrapped(Bunch.GetNumBits(), UNetConnection::MaxPacket * 8);
	check(!SendBunchHeader.IsError());

	// Remember start position.
	AllowMerge      = InAllowMerge;
	Bunch.Time      = Driver->Time;

	if ((Bunch.bClose || Bunch.bOpen) && UE_LOG_ACTIVE(LogNetDormancy,VeryVerbose) )
	{
		UE_LOG(LogNetDormancy, VeryVerbose, TEXT("Sending: %s"), *Bunch.ToString());
	}

	if (UE_LOG_ACTIVE(LogNetTraffic,VeryVerbose))
	{
		UE_LOG(LogNetTraffic, VeryVerbose, TEXT("Sending: %s"), *Bunch.ToString());
	}

	NETWORK_PROFILER(GNetworkProfiler.PushSendBunch(this, &Bunch, SendBunchHeader.GetNumBits(), Bunch.GetNumBits()));

	// Write the bits to the buffer and remember the packet id used
	Bunch.PacketId = WriteBitsToSendBuffer( SendBunchHeader.GetData(), SendBunchHeader.GetNumBits(), Bunch.GetData(), Bunch.GetNumBits(), EWriteBitsDataType::Bunch );

	// Track channels that wrote data to this packet.
	FChannelRecordImpl::PushChannelRecord(ChannelRecord, Bunch.PacketId, Bunch.ChIndex);

	UE_LOG(LogNetTraffic, Verbose, TEXT("UNetConnection::SendRawBunch. ChIndex: %d. Bits: %d. PacketId: %d"), Bunch.ChIndex, Bunch.GetNumBits(), Bunch.PacketId );

	if ( PackageMap && Bunch.bHasPackageMapExports )
	{
		PackageMap->NotifyBunchCommit( Bunch.PacketId, &Bunch );
	}

	if ( Bunch.bHasPackageMapExports )
	{
		Driver->NetGUIDOutBytes += (SendBunchHeader.GetNumBits() + Bunch.GetNumBits()) >> 3;
	}

	return Bunch.PacketId;
}

UChannel* UNetConnection::CreateChannel( EChannelType Type, bool bOpenedLocally, int32 ChannelIndex )
{
	const EChannelCreateFlags ChannelCreateFlags = (bOpenedLocally ? EChannelCreateFlags::OpenedLocally : EChannelCreateFlags::None);
	FName ChName = NAME_None;

	switch (Type)
	{
	case CHTYPE_Control:
		ChName = NAME_Control;
		break;
	case CHTYPE_Actor:
		ChName = NAME_Actor;
		break;
	case CHTYPE_Voice:
		ChName = NAME_Voice;
		break;
	}

	return CreateChannelByName(ChName, ChannelCreateFlags, ChannelIndex);
}

UChannel* UNetConnection::CreateChannelByName( const FName& ChName, EChannelCreateFlags CreateFlags, int32 ChIndex )
{
	check(Driver->IsKnownChannelName(ChName));
	AssertValid();

	// If no channel index was specified, find the first available.
	if( ChIndex==INDEX_NONE )
	{
		int32 FirstChannel = 1;

		int32 StaticChannelIndex = Driver->ChannelDefinitionMap[ChName].StaticChannelIndex;
		if (StaticChannelIndex != INDEX_NONE)
		{
			FirstChannel = StaticChannelIndex;
		}

		// Search the channel array for an available location
		for( ChIndex=FirstChannel; ChIndex < Channels.Num(); ChIndex++ )
		{
			if( !Channels[ChIndex] )
			{
				break;
			}
		}
		
		// Fail to create if the channel array is full
		if( ChIndex == Channels.Num() )
		{
			if (!bHasWarnedAboutChannelLimit)
			{
				bHasWarnedAboutChannelLimit = true;
				UE_LOG(LogNetTraffic, Warning, TEXT("No free channel could be found in the channel list (current limit is %d channels) for connection with owner %s. Consider increasing the max channels allowed using CVarMaxChannelSize."), MaxChannelSize, *GetNameSafe(OwningActor));
			}
			return NULL;
		}
	}

	// Make sure channel is valid.
	check(ChIndex < Channels.Num());
	check(Channels[ChIndex] == NULL);

	// Create channel.
	UChannel* Channel = Driver->GetOrCreateChannelByName(ChName);
	check(Channel);
	Channel->Init( this, ChIndex, CreateFlags );
	Channels[ChIndex] = Channel;
	OpenChannels.Add(Channel);

	if (Driver->ChannelDefinitionMap[ChName].bTickOnCreate)
	{
		StartTickingChannel(Channel);
	}

	UE_LOG(LogNetTraffic, Log, TEXT("Created channel %i of type %s"), ChIndex, *ChName.ToString());

	return Channel;
}

/**
 * @return Finds the voice channel for this connection or NULL if none
 */
UVoiceChannel* UNetConnection::GetVoiceChannel()
{
	check(Driver);
	if (!Driver->IsKnownChannelName(NAME_Voice))
	{
		return nullptr;
	}

	int32 VoiceChannelIndex = Driver->ChannelDefinitionMap[NAME_Voice].StaticChannelIndex;
	check(Channels.IsValidIndex(VoiceChannelIndex));

	return Channels[VoiceChannelIndex] != nullptr && Channels[VoiceChannelIndex]->ChName == NAME_Voice ?
		Cast<UVoiceChannel>(Channels[VoiceChannelIndex]) : nullptr;
}

float UNetConnection::GetTimeoutValue()
{
	check(Driver);
#if !UE_BUILD_SHIPPING
	if (Driver->bNoTimeouts)
	{
		// APlayerController depends on this timeout to destroy itself and free up
		// its resources, so we have to handle this case here as well
		return bPendingDestroy ? 2.f : MAX_FLT;
	}
#endif

	float Timeout = Driver->InitialConnectTimeout;

	if ((State != USOCK_Pending) && (bPendingDestroy || (OwningActor && OwningActor->UseShortConnectTimeout())))
	{
		const float ConnectionTimeout = Driver->ConnectionTimeout;

		// If the connection is pending destroy give it 2 seconds to try to finish sending any reliable packets
		Timeout = bPendingDestroy ? 2.f : ConnectionTimeout;
	}

	// Longtimeouts allows a multiplier to be added to get correct disconnection behavior
	// with with additional leniancy when required. Implicit in debug/editor builds
	static bool LongTimeouts = FParse::Param(FCommandLine::Get(), TEXT("longtimeouts"));

	if (Driver->TimeoutMultiplierForUnoptimizedBuilds > 0 
		&& (LongTimeouts || WITH_EDITOR || UE_BUILD_DEBUG)
		)
	{
		Timeout *= Driver->TimeoutMultiplierForUnoptimizedBuilds;
	}

	return Timeout;
}

void UNetConnection::Tick()
{
	SCOPE_CYCLE_COUNTER(Stat_NetConnectionTick);

	AssertValid();

	// Lag simulation.
#if DO_ENABLE_NET_TEST
	if( PacketSimulationSettings.PktLag )
	{
		for( int32 i=0; i < Delayed.Num(); i++ )
		{
			if( FPlatformTime::Seconds() > Delayed[i].SendTime )
			{
				LowLevelSend((char*)&Delayed[i].Data[0], Delayed[i].SizeBits, Delayed[i].Traits);
				Delayed.RemoveAt( i );
				i--;
			}
			else
			{
				// Break now instead of continuing to iterate through the list. Otherwise LagVariance may cause out of order sends
				break;
			}
		}
	}
#endif

	// Get frame time.
	const double CurrentRealtimeSeconds = FPlatformTime::Seconds();

	// if this is 0 it's our first tick since init, so start our real-time tracking from here
	if (LastTime == 0.0)
	{
		LastTime = CurrentRealtimeSeconds;
		LastReceiveRealtime = CurrentRealtimeSeconds;
		LastGoodPacketRealtime = CurrentRealtimeSeconds;
	}

	FrameTime = CurrentRealtimeSeconds - LastTime;
	const int32 MaxNetTickRate = Driver->MaxNetTickRate;
	float EngineTickRate = GEngine->GetMaxTickRate(0.0f, false);
	// We want to make sure the DesiredTickRate stays at <= 0 if there's no tick rate limiting of any kind, since it's used later in the function for bandwidth limiting.
	if (MaxNetTickRate > 0 && EngineTickRate <= 0.0f)
	{
		EngineTickRate = MAX_flt;
	}
	const float MaxNetTickRateFloat = MaxNetTickRate > 0 ? float(MaxNetTickRate) : MAX_flt;
	const float DesiredTickRate = FMath::Clamp(EngineTickRate, 0.0f, MaxNetTickRateFloat);
	// Apply net tick rate limiting if the desired net tick rate is strictly less than the engine tick rate.
	if (!InternalAck && MaxNetTickRateFloat < EngineTickRate && DesiredTickRate > 0.0f)
	{
		const float MinNetFrameTime = 1.0f/DesiredTickRate;
		if (FrameTime < MinNetFrameTime)
		{
			return;
		}
	}

	LastTime = CurrentRealtimeSeconds;
	CumulativeTime += FrameTime;
	CountedFrames++;
	if(CumulativeTime > 1.f)
	{
		AverageFrameTime = CumulativeTime / CountedFrames;
		CumulativeTime = 0;
		CountedFrames = 0;
	}

	// Pretend everything was acked, for 100% reliable connections or demo recording.
	if( InternalAck )
	{
		const bool bIsServer = Driver->IsServer();
		OutAckPacketId = OutPacketId;

		LastReceiveTime = Driver->Time;
		LastReceiveRealtime = FPlatformTime::Seconds();
		LastGoodPacketRealtime = FPlatformTime::Seconds();

		// Consume all records
		auto InternnalAckChannelFunc = [this, bIsServer](uint32 ChannelIndex)
		{
			UChannel* Channel = Channels[ChannelIndex];
			if (Channel)
			{
				for(FOutBunch* OutBunch=Channel->OutRec; OutBunch; OutBunch=OutBunch->Next)
				{
					OutBunch->ReceivedAck = 1;
				}

				if (bIsServer || Channel->OpenedLocally)
				{
					Channel->OpenAcked = 1;
				}

				Channel->ReceivedAcks();
			}
		};

		FChannelRecordImpl::ConsumeAllChannelRecords(ChannelRecord, InternnalAckChannelFunc);
	}

	// Update stats.
	if ( CurrentRealtimeSeconds - StatUpdateTime > StatPeriod )
	{
		// Update stats.
		const float RealTime = CurrentRealtimeSeconds - StatUpdateTime;
		if( LagCount )
		{
			AvgLag = LagAcc/LagCount;
		}
		BestLag = AvgLag;

		InBytesPerSecond = FMath::TruncToInt(static_cast<float>(InBytes) / RealTime);
		OutBytesPerSecond = FMath::TruncToInt(static_cast<float>(OutBytes) / RealTime);
		InPacketsPerSecond = FMath::TruncToInt(static_cast<float>(InPackets) / RealTime);
		OutPacketsPerSecond = FMath::TruncToInt(static_cast<float>(OutPackets) / RealTime);

		// Init counters.
		LagAcc = 0;
		StatUpdateTime = CurrentRealtimeSeconds;
		BestLagAcc = 9999;
		LagCount = 0;
		InPacketsLost = 0;
		OutPacketsLost = 0;
		InBytes = 0;
		OutBytes = 0;
		InPackets = 0;
		OutPackets = 0;
	}

	if (bConnectionPendingCloseDueToSocketSendFailure)
	{
		Close();
		bConnectionPendingCloseDueToSocketSendFailure = false;

		// early out
		return;
	}

	// Compute time passed since last update.
	const float DeltaTime	= Driver->Time - LastTickTime;
	LastTickTime			= Driver->Time;

	// Handle timeouts.
	const float Timeout = GetTimeoutValue();

	if ((CurrentRealtimeSeconds - LastReceiveRealtime) > Timeout)
	{
		const TCHAR* const TimeoutString = TEXT("UNetConnection::Tick: Connection TIMED OUT. Closing connection.");
		const TCHAR* const DestroyString = TEXT("UNetConnection::Tick: Connection closing during pending destroy, not all shutdown traffic may have been negotiated");
		
		// Compute true realtime since packet was received (as well as truly processed)
		const double Seconds = FPlatformTime::Seconds();

		const float ReceiveRealtimeDelta = Seconds - LastReceiveRealtime;
		const float GoodRealtimeDelta = Seconds - LastGoodPacketRealtime;

		// Timeout.
		FString Error = FString::Printf(TEXT("%s. Elapsed: %2.2f, Real: %2.2f, Good: %2.2f, DriverTime: %2.2f, Threshold: %2.2f, %s"),
			bPendingDestroy ? DestroyString : TimeoutString,
			Driver->Time - LastReceiveTime,
			ReceiveRealtimeDelta,
			GoodRealtimeDelta,
			Driver->Time,
			Timeout,
			*Describe());
		
		static double LastTimePrinted = 0.0f;
		if (FPlatformTime::Seconds() - LastTimePrinted > GEngine->NetErrorLogInterval)
		{
			UE_LOG(LogNet, Warning, TEXT("%s"), *Error);
			LastTimePrinted = FPlatformTime::Seconds();
		}

		if (!bPendingDestroy)
		{
			GEngine->BroadcastNetworkFailure(Driver->GetWorld(), Driver, ENetworkFailure::ConnectionTimeout, Error);
		}

		Close();
#if USE_SERVER_PERF_COUNTERS
		PerfCountersIncrement(TEXT("TimedoutConnections"));
#endif

		if (Driver == NULL)
		{
			// Possible that the Broadcast above caused someone to kill the net driver, early out
			return;
		}
	}
	else
	{
		// We should never need more ticking channels than open channels
		checkf(ChannelsToTick.Num() <= OpenChannels.Num(), TEXT("More ticking channels (%d) than open channels (%d) for net connection!"), ChannelsToTick.Num(), OpenChannels.Num())

		// Tick the channels.
		if (CVarTickAllOpenChannels.GetValueOnAnyThread() == 0)
		{
			for( int32 i=ChannelsToTick.Num()-1; i>=0; i-- )
			{
				ChannelsToTick[i]->Tick();

				if (ChannelsToTick[i]->CanStopTicking())
				{
					ChannelsToTick.RemoveAt(i);
				}
			}
		}
		else
		{
			for (int32 i = OpenChannels.Num() - 1; i >= 0; i--)
			{
				if (OpenChannels[i])
				{
					OpenChannels[i]->Tick();
				}
				else
				{
					UE_LOG(LogNet, Warning, TEXT("UNetConnection::Tick: null channel in OpenChannels array. %s"), *Describe());
				}
			}
		}

		for ( auto ProcessingActorMapIter = KeepProcessingActorChannelBunchesMap.CreateIterator(); ProcessingActorMapIter; ++ProcessingActorMapIter )
		{
			TArray<UActorChannel*>& ActorChannelArray = ProcessingActorMapIter.Value();
			for ( int32 ActorChannelIdx = 0; ActorChannelIdx < ActorChannelArray.Num(); ++ActorChannelIdx )
			{
				UActorChannel* CurChannel = ActorChannelArray[ActorChannelIdx];
				
				bool bRemoveChannel = false;
				if ( CurChannel && !CurChannel->IsPendingKill() )
				{
					check( CurChannel->ChIndex == -1 );
					if ( CurChannel->ProcessQueuedBunches() )
					{
						// Since we are done processing bunches, we can now actually clean this channel up
						CurChannel->ConditionalCleanUp(false, CurChannel->QueuedCloseReason);

						bRemoveChannel = true;
						UE_LOG( LogNet, VeryVerbose, TEXT("UNetConnection::Tick: Removing from KeepProcessingActorChannelBunchesMap. Num: %i"), KeepProcessingActorChannelBunchesMap.Num() );
					}

				}
				else
				{
					bRemoveChannel = true;
					UE_LOG( LogNet, Verbose, TEXT("UNetConnection::Tick: Removing from KeepProcessingActorChannelBunchesMap before done processing bunches. Num: %i"), KeepProcessingActorChannelBunchesMap.Num() );
				}

				// Remove the actor channel from the array
				if ( bRemoveChannel )
				{
					ActorChannelArray.RemoveAt( ActorChannelIdx, 1, false );
					--ActorChannelIdx;
				}
			}

			if ( ActorChannelArray.Num() == 0 )
			{
				ProcessingActorMapIter.RemoveCurrent();
			}
		}

		// If channel 0 has closed, mark the connection as closed.
		if (Channels[0] == nullptr && (OutReliable[0] != InitOutReliable || InReliable[0] != InitInReliable))
		{
			State = USOCK_Closed;
		}
	}

	// Flush.
	if ( TimeSensitive || (Driver->Time - LastSendTime) > Driver->KeepAliveTime)
	{
		bool bHandlerHandshakeComplete = !Handler.IsValid() || Handler->IsFullyInitialized();

		// Delay any packet sends on the server, until we've verified that a packet has been received from the client.
		if (bHandlerHandshakeComplete && HasReceivedClientPacket())
		{
			FlushNet();
		}
	}

	// Tick Handler
	if (Handler.IsValid())
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_NetConnection_TickPacketHandler)

		Handler->Tick(FrameTime);

		// Resend any queued up raw packets (these come from the reliability handler)
		BufferedPacket* ResendPacket = Handler->GetQueuedRawPacket();

		if (ResendPacket && Driver->IsNetResourceValid())
		{
			Handler->SetRawSend(true);

			while (ResendPacket != nullptr)
			{
				LowLevelSend(ResendPacket->Data, ResendPacket->CountBits, ResendPacket->Traits);
				ResendPacket = Handler->GetQueuedRawPacket();
			}

			Handler->SetRawSend(false);
		}

		BufferedPacket* QueuedPacket = Handler->GetQueuedPacket();

		/* Send all queued packets */
		while(QueuedPacket != nullptr)
		{
			if (Driver->IsNetResourceValid())
			{
				LowLevelSend(QueuedPacket->Data, QueuedPacket->CountBits, QueuedPacket->Traits);
			}
			delete QueuedPacket;
			QueuedPacket = Handler->GetQueuedPacket();
		}
	}

	// Update queued byte count.
	// this should be at the end so that the cap is applied *after* sending (and adjusting QueuedBytes for) any remaining data for this tick

	SaturationAnalytics.TrackFrame(!IsNetReady(false));

	// Clamp DeltaTime for bandwidth limiting so that if there is a hitch, we don't try to send
	// a large burst on the next frame, which can cause another hitch if a lot of additional replication occurs.
	float BandwidthDeltaTime = DeltaTime;
	if (DesiredTickRate != 0.0f)
	{
		BandwidthDeltaTime = FMath::Clamp(BandwidthDeltaTime, 0.0f, 1.0f / DesiredTickRate);
	}

	float DeltaBits = CurrentNetSpeed * BandwidthDeltaTime * 8.f;
	QueuedBits -= FMath::TruncToInt(DeltaBits);
	float AllowedLag = 2.f * DeltaBits;
	if (QueuedBits < -AllowedLag)
	{
		QueuedBits = FMath::TruncToInt(-AllowedLag);
	}
}

void UNetConnection::HandleClientPlayer( APlayerController *PC, UNetConnection* NetConnection )
{
	check(Driver->GetWorld());

	// Hook up the Viewport to the new player actor.
	ULocalPlayer*	LocalPlayer = NULL;
	for(FLocalPlayerIterator It(GEngine, Driver->GetWorld());It;++It)
	{
		LocalPlayer = *It;
		break;
	}

	// Detach old player if it's in the same level.
	check(LocalPlayer);
	if( LocalPlayer->PlayerController && LocalPlayer->PlayerController->GetLevel() == PC->GetLevel())
	{
		if (LocalPlayer->PlayerController->Role == ROLE_Authority)
		{
			// local placeholder PC while waiting for connection to be established
			LocalPlayer->PlayerController->GetWorld()->DestroyActor(LocalPlayer->PlayerController);
		}
		else
		{
			// tell the server the swap is complete
			// we cannot use a replicated function here because the server has already transferred ownership and will reject it
			// so use a control channel message
			int32 Index = INDEX_NONE;
			FNetControlMessage<NMT_PCSwap>::Send(this, Index);
		}
		LocalPlayer->PlayerController->Player = NULL;
		LocalPlayer->PlayerController->NetConnection = NULL;
		LocalPlayer->PlayerController = NULL;
	}

	LocalPlayer->CurrentNetSpeed = CurrentNetSpeed;

	// Init the new playerpawn.
	PC->Role = ROLE_AutonomousProxy;
	PC->NetConnection = NetConnection;
	PC->SetPlayer(LocalPlayer);
	UE_LOG(LogNet, Verbose, TEXT("%s setplayer %s"),*PC->GetName(),*LocalPlayer->GetName());
	LastReceiveTime = Driver->Time;
	State = USOCK_Open;
	PlayerController = PC;
	OwningActor = PC;

	UWorld* World = PlayerController->GetWorld();
	// if we have already loaded some sublevels, tell the server about them
	{
		TArray<FUpdateLevelVisibilityLevelInfo> LevelVisibilities;
		for (ULevelStreaming* LevelStreaming : World->GetStreamingLevels())
		{
			if (LevelStreaming)
			{
				const ULevel* Level = LevelStreaming->GetLoadedLevel();
				if ( Level && Level->bIsVisible && !Level->bClientOnlyVisible )
				{
					FUpdateLevelVisibilityLevelInfo& LevelVisibility = *new( LevelVisibilities ) FUpdateLevelVisibilityLevelInfo();
					LevelVisibility.PackageName = PC->NetworkRemapPath(Level->GetOutermost()->GetFName(), false);
					LevelVisibility.bIsVisible = true;
				}
			}
		}
		if( LevelVisibilities.Num() > 0 )
		{
			PC->ServerUpdateMultipleLevelsVisibility( LevelVisibilities );
		}
	}

	// if we have splitscreen viewports, ask the server to join them as well
	bool bSkippedFirst = false;
	for (FLocalPlayerIterator It(GEngine, Driver->GetWorld()); It; ++It)
	{
		if (*It != LocalPlayer)
		{
			// send server command for new child connection
			It->SendSplitJoin();
		}
	}
}

void UChildConnection::HandleClientPlayer(APlayerController* PC, UNetConnection* NetConnection)
{
	// find the first player that doesn't already have a connection
	ULocalPlayer* NewPlayer = NULL;
	uint8 CurrentIndex = 0;
	for (FLocalPlayerIterator It(GEngine, Driver->GetWorld()); It; ++It, CurrentIndex++)
	{
		if (CurrentIndex == PC->NetPlayerIndex)
		{
			NewPlayer = *It;
			break;
		}
	}

	if (!ensure(NewPlayer != NULL))
	{
		UE_LOG(LogNet, Error, TEXT("Failed to find LocalPlayer for received PlayerController '%s' with index %d. PlayerControllers:"), *PC->GetName(), int32(PC->NetPlayerIndex));

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		check( PC->GetWorld() );
		for (TActorIterator<APlayerController> It(PC->GetWorld()); It; ++It)
		{
			if (It->Role < ROLE_Authority)
			{
				UE_LOG(LogNet, Log, TEXT(" - %s"), *It->GetFullName());
			}
		}
#endif
		if (ensure(Parent != nullptr))
		{
			Parent->Close();
		}
		return; // avoid crash
	}

	// Detach old player.
	check(NewPlayer);
	if (NewPlayer->PlayerController != NULL)
	{
		if (NewPlayer->PlayerController->Role == ROLE_Authority)
		{
			// local placeholder PC while waiting for connection to be established
			NewPlayer->PlayerController->GetWorld()->DestroyActor(NewPlayer->PlayerController);
		}
		else
		{
			// tell the server the swap is complete
			// we cannot use a replicated function here because the server has already transferred ownership and will reject it
			// so use a control channel message
			int32 Index = Parent->Children.Find(this);
			FNetControlMessage<NMT_PCSwap>::Send(Parent, Index);
		}
		NewPlayer->PlayerController->Player = NULL;
		NewPlayer->PlayerController->NetConnection = NULL;
		NewPlayer->PlayerController = NULL;
	}

	NewPlayer->CurrentNetSpeed = CurrentNetSpeed;

	// Init the new playerpawn.
	PC->Role = ROLE_AutonomousProxy;
	PC->NetConnection = NetConnection;
	PC->SetPlayer(NewPlayer);
	UE_LOG(LogNet, Verbose, TEXT("%s setplayer %s"), *PC->GetName(), *NewPlayer->GetName());
	PlayerController = PC;
	OwningActor = PC;
}

#if DO_ENABLE_NET_TEST

void UNetConnection::UpdatePacketSimulationSettings(void)
{
	check(Driver);
	PacketSimulationSettings.PktLoss = Driver->PacketSimulationSettings.PktLoss;
	PacketSimulationSettings.PktLossMinSize = Driver->PacketSimulationSettings.PktLossMinSize;
	PacketSimulationSettings.PktLossMaxSize = Driver->PacketSimulationSettings.PktLossMaxSize;
	PacketSimulationSettings.PktOrder = Driver->PacketSimulationSettings.PktOrder;
	PacketSimulationSettings.PktDup = Driver->PacketSimulationSettings.PktDup;
	PacketSimulationSettings.PktLag = Driver->PacketSimulationSettings.PktLag;
	PacketSimulationSettings.PktLagVariance = Driver->PacketSimulationSettings.PktLagVariance;
}
#endif

/**
 * Called to determine if a voice packet should be replicated to this
 * connection or any of its child connections
 *
 * @param Sender - the sender of the voice packet
 *
 * @return true if it should be sent on this connection, false otherwise
 */
bool UNetConnection::ShouldReplicateVoicePacketFrom(const FUniqueNetId& Sender)
{
	if (PlayerController &&
		// Has the handshaking of the mute list completed?
		PlayerController->MuteList.bHasVoiceHandshakeCompleted)
	{	
		// Check with the owning player controller first.
		if (Sender.IsValid() &&
			// Determine if the server should ignore replication of voice packets that are already handled by a peer connection
			//(!Driver->AllowPeerVoice || !Actor->HasPeerConnection(Sender)) &&
			// Determine if the sender was muted for the local player 
			PlayerController->IsPlayerMuted(Sender) == false)
		{
			// The parent wants to allow, but see if any child connections want to mute
			for (int32 Index = 0; Index < Children.Num(); Index++)
			{
				if (Children[Index]->ShouldReplicateVoicePacketFrom(Sender) == false)
				{
					// A child wants to mute, so skip
					return false;
				}
			}
			// No child wanted to block it so accept
			return true;
		}
	}
	// Not able to handle voice yet or player is muted on this connection
	return false;
}

void UNetConnection::ResetGameWorldState()
{
	//Clear out references and do whatever else so that nothing holds onto references that it doesn't need to.
	ResetDestructionInfos();
	ClientVisibleLevelNames.Empty();
	KeepProcessingActorChannelBunchesMap.Empty();
	DormantReplicatorMap.Empty();
	CleanupDormantActorState();
}

void UNetConnection::CleanupDormantActorState()
{
	DormantReplicatorMap.Empty();
}

void UNetConnection::FlushDormancy(class AActor* Actor)
{
	UE_LOG( LogNetDormancy, Verbose, TEXT( "FlushDormancy: %s. Connection: %s" ), *Actor->GetName(), *GetName() );
	
	if ( Driver->GetNetworkObjectList().MarkActive( Actor, this, Driver ) )
	{
		FlushDormancyForObject( Actor );

		for ( UActorComponent* ActorComp : Actor->GetReplicatedComponents() )
		{
			if ( ActorComp && ActorComp->GetIsReplicated() )
			{
				FlushDormancyForObject( ActorComp );
			}
		}
	}

	// If channel is pending dormancy, cancel it
			
	// If the close bunch was already sent, that is fine, by reseting the dormant flag
	// here, the server will not add the actor to the dormancy list when he closes the channel 
	// after he gets the client ack. The result is the channel will close but be open again
	// right away
	UActorChannel* Ch = FindActorChannelRef(Actor);

	if ( Ch != nullptr )
	{
		UE_LOG( LogNetDormancy, Verbose, TEXT( "    Found Channel[%d] '%s'. Reseting Dormancy. Ch->Closing: %d" ), Ch->ChIndex, *Ch->Describe(), Ch->Closing );

		Ch->Dormant = false;
		Ch->bPendingDormancy = false;
	}

}

void UNetConnection::ForcePropertyCompare( AActor* Actor )
{
	UActorChannel* Ch = FindActorChannelRef( Actor );

	if ( Ch != nullptr )
	{
		Ch->bForceCompareProperties = true;
	}
}

/** Wrapper for validating an objects dormancy state, and to prepare the object for replication again */
void UNetConnection::FlushDormancyForObject( UObject* Object )
{
	const bool ValidateProperties = (GNetDormancyValidate == 1);

	TSharedRef< FObjectReplicator > * Replicator = DormantReplicatorMap.Find( Object );

	if ( Replicator != NULL )
	{
		if ( ValidateProperties )
		{
			Replicator->Get().ValidateAgainstState( Object );
		}

		DormantReplicatorMap.Remove( Object );

		// Set to NULL to force a new replicator to be created using the objects current state
		// It's totally possible to let this replicator fall through, and continue on where we left off 
		// which could send all the changed properties since this object went dormant
		Replicator = NULL;	
	}

	if ( Replicator == NULL )
	{
		Replicator = &DormantReplicatorMap.Add( Object, TSharedRef<FObjectReplicator>( new FObjectReplicator() ) );

		Replicator->Get().InitWithObject( Object, this, false );		// Init using the objects current state

		// Flush the must be mapped GUIDs, the initialization may add them, but they're phantom and will be remapped when actually sending
		UPackageMapClient * PackageMapClient = CastChecked< UPackageMapClient >(PackageMap);

		if (PackageMapClient)
		{
			TArray< FNetworkGUID >& MustBeMappedGuidsInLastBunch = PackageMapClient->GetMustBeMappedGuidsInLastBunch();
			MustBeMappedGuidsInLastBunch.Empty();
		}	
	}
}

/** Wrapper for setting the current client login state, so we can trap for debugging, and verbosity purposes. */
void UNetConnection::SetClientLoginState( const EClientLoginState::Type NewState )
{
	if ( ClientLoginState == NewState )
	{
		UE_LOG(LogNet, Verbose, TEXT("UNetConnection::SetClientLoginState: State same: %s"), EClientLoginState::ToString( NewState ) );
		return;
	}

	UE_CLOG((Driver == nullptr || !Driver->DDoS.CheckLogRestrictions()), LogNet, Verbose,
				TEXT("UNetConnection::SetClientLoginState: State changing from %s to %s"),
				EClientLoginState::ToString(ClientLoginState), EClientLoginState::ToString(NewState));

	ClientLoginState = NewState;
}

/** Wrapper for setting the current expected client login msg type. */
void UNetConnection::SetExpectedClientLoginMsgType(const uint8 NewType)
{
	const bool bLogRestricted = Driver != nullptr && Driver->DDoS.CheckLogRestrictions();

	if ( ExpectedClientLoginMsgType == NewType )
	{
		UE_CLOG(!bLogRestricted, LogNet, Verbose, TEXT("UNetConnection::SetExpectedClientLoginMsgType: Type same: [%d]%s"),
				NewType, FNetControlMessageInfo::IsRegistered(NewType) ? FNetControlMessageInfo::GetName(NewType) : TEXT("UNKNOWN"));

		return;
	}

	UE_CLOG(!bLogRestricted, LogNet, Verbose,
		TEXT("UNetConnection::SetExpectedClientLoginMsgType: Type changing from [%d]%s to [%d]%s"), 
		ExpectedClientLoginMsgType,
		FNetControlMessageInfo::IsRegistered(ExpectedClientLoginMsgType) ? FNetControlMessageInfo::GetName(ExpectedClientLoginMsgType) : TEXT("UNKNOWN"),
		NewType,
		FNetControlMessageInfo::IsRegistered(NewType) ? FNetControlMessageInfo::GetName(NewType) : TEXT("UNKNOWN"));

	ExpectedClientLoginMsgType = NewType;
}

/** This function validates that ClientMsgType is the next expected msg type. */
bool UNetConnection::IsClientMsgTypeValid( const uint8 ClientMsgType )
{
	if ( ClientLoginState == EClientLoginState::LoggingIn )
	{
		// If client is logging in, we are expecting a certain msg type each step of the way
		if ( ClientMsgType != ExpectedClientLoginMsgType )
		{
			// Not the expected msg type
			UE_LOG(LogNet, Log, TEXT("UNetConnection::IsClientMsgTypeValid FAILED: (ClientMsgType != ExpectedClientLoginMsgType) Remote Address=%s"), *LowLevelGetRemoteAddress());
			return false;
		}
	} 
	else
	{
		// Once a client is logged in, we no longer expect any of the msg types below
		if ( ClientMsgType == NMT_Hello || ClientMsgType == NMT_Login )
		{
			// We don't want to see these msg types once the client is fully logged in
			UE_LOG(LogNet, Log, TEXT("UNetConnection::IsClientMsgTypeValid FAILED: Invalid msg after being logged in - Remote Address=%s"), *LowLevelGetRemoteAddress());
			return false;
		}
	}

	return true;
}

/**
* This function tracks the number of log calls per second for this client, 
* and disconnects the client if it detects too many calls are made per second
*/
bool UNetConnection::TrackLogsPerSecond()
{
	const double NewTime = FPlatformTime::Seconds();

	const double LogCallTotalTime = NewTime - LogCallLastTime;

	LogCallCount++;

	static const double LOG_AVG_THRESHOLD				= 0.5;		// Frequency to check threshold
	static const double	MAX_LOGS_PER_SECOND_INSTANT		= 60;		// If they hit this limit, they will instantly get disconnected
	static const double	MAX_LOGS_PER_SECOND_SUSTAINED	= 5;		// If they sustain this logs/second for a certain count, they get disconnected
	static const double	MAX_SUSTAINED_COUNT				= 10;		// If they sustain MAX_LOGS_PER_SECOND_SUSTAINED for this count, they get disconnected (5 seconds currently)

	if ( LogCallTotalTime > LOG_AVG_THRESHOLD )
	{
		const double LogsPerSecond = (double)LogCallCount / LogCallTotalTime;

		LogCallLastTime = NewTime;
		LogCallCount	= 0;

		if ( LogsPerSecond > MAX_LOGS_PER_SECOND_INSTANT )
		{
			// Hit this instant limit, we instantly disconnect them
			UE_LOG( LogNet, Warning, TEXT( "UNetConnection::TrackLogsPerSecond instant FAILED. LogsPerSecond: %f, RemoteAddr: %s" ), (float)LogsPerSecond, *LowLevelGetRemoteAddress() );
			Close();		// Close the connection

#if USE_SERVER_PERF_COUNTERS
			PerfCountersIncrement(TEXT("ClosedConnectionsDueToMaxBadRPCsLimit"));
#endif
			return false;
		}

		if ( LogsPerSecond > MAX_LOGS_PER_SECOND_SUSTAINED )
		{
			// Hit the sustained limit, count how many times we get here
			LogSustainedCount++;

			// Warn that we are approaching getting disconnected (will be useful when going over historical logs)
			UE_LOG( LogNet, Warning, TEXT( "UNetConnection::TrackLogsPerSecond: LogsPerSecond > MAX_LOGS_PER_SECOND_SUSTAINED. LogSustainedCount: %i, LogsPerSecond: %f, RemoteAddr: %s" ), LogSustainedCount, (float)LogsPerSecond, *LowLevelGetRemoteAddress() );

			if ( LogSustainedCount > MAX_SUSTAINED_COUNT )
			{
				// Hit the sustained limit for too long, disconnect them
				UE_LOG( LogNet, Warning, TEXT( "UNetConnection::TrackLogsPerSecond: LogSustainedCount > MAX_SUSTAINED_COUNT. LogsPerSecond: %f, RemoteAddr: %s" ), (float)LogsPerSecond, *LowLevelGetRemoteAddress() );
				Close();		// Close the connection

#if USE_SERVER_PERF_COUNTERS
				PerfCountersIncrement(TEXT("ClosedConnectionsDueToMaxBadRPCsLimit"));
#endif
				return false;
			}
		}
		else
		{
			// Reset sustained count since they are not above the threshold
			LogSustainedCount = 0;
		}
	}

	return true;
}

void UNetConnection::ResetPacketBitCounts()
{
	NumPacketIdBits = 0;
	NumBunchBits = 0;
	NumAckBits = 0;
	NumPaddingBits = 0;
}

void UNetConnection::SetPlayerOnlinePlatformName(const FName InPlayerOnlinePlatformName)
{
	PlayerOnlinePlatformName = InPlayerOnlinePlatformName;
}

void UNetConnection::DestroyIgnoredActor(AActor* Actor)
{
	if (Driver && Driver->World)
	{
		Driver->World->DestroyActor(Actor, true);
	}
}

void UNetConnection::CleanupDormantReplicatorsForActor(AActor* Actor)
{
	if (Actor)
	{
		DormantReplicatorMap.Remove(Actor);
		for (UActorComponent* const Component : Actor->GetReplicatedComponents())
		{
			DormantReplicatorMap.Remove(Component);
		}
	}
}

void UNetConnection::CleanupStaleDormantReplicators()
{
	for (auto It = DormantReplicatorMap.CreateIterator(); It; ++It)
	{
		if (!It.Value()->GetWeakObjectPtr().IsValid())
		{
			It.RemoveCurrent();
		}
	}
}


void UNetConnection::SetPendingCloseDueToSocketSendFailure()
{
	bConnectionPendingCloseDueToSocketSendFailure = true;
}

void UNetConnection::ConsumeQueuedActorDelinquencyAnalytics(FNetQueuedActorDelinquencyAnalytics& Out)
{
	if (UPackageMapClient* PackageMapClient = Cast<UPackageMapClient>(PackageMap))
	{
		return PackageMapClient->ConsumeQueuedActorDelinquencyAnalytics(Out);
	}
	else
	{
		Out.Reset();
	}
}

const FNetQueuedActorDelinquencyAnalytics& UNetConnection::GetQueuedActorDelinquencyAnalytics() const
{
	static FNetQueuedActorDelinquencyAnalytics Empty;

	if (UPackageMapClient const * const PackageMapClient = Cast<UPackageMapClient>(PackageMap))
	{
		return PackageMapClient->GetQueuedActorDelinquencyAnalytics();
	}
	
	return Empty;
}

void UNetConnection::ResetQueuedActorDelinquencyAnalytics()
{
	if (UPackageMapClient* PackageMapClient = Cast<UPackageMapClient>(PackageMap))
	{
		PackageMapClient->ResetQueuedActorDelinquencyAnalytics();
	}
}

void UNetConnection::ConsumeSaturationAnalytics(FNetConnectionSaturationAnalytics& Out)
{
	Out = MoveTemp(SaturationAnalytics);
	SaturationAnalytics.Reset();
}

const FNetConnectionSaturationAnalytics& UNetConnection::GetSaturationAnalytics() const
{
	return SaturationAnalytics;
}

void UNetConnection::ResetSaturationAnalytics()
{
	SaturationAnalytics.Reset();
}

void UNetConnection::TrackReplicationForAnalytics(const bool bWasSaturated)
{
	++TickCount;
	SaturationAnalytics.TrackReplication(bWasSaturated);
}

/*-----------------------------------------------------------------------------
	USimulatedClientNetConnection.
-----------------------------------------------------------------------------*/

USimulatedClientNetConnection::USimulatedClientNetConnection( const FObjectInitializer& ObjectInitializer ) : Super( ObjectInitializer )
{
	InternalAck = true;
}

void USimulatedClientNetConnection::HandleClientPlayer( class APlayerController* PC, class UNetConnection* NetConnection )
{
	State = USOCK_Open;
	PlayerController = PC;
	OwningActor = PC;
}

// ----------------------------------------------------------------

static void	AddSimulatedNetConnections(const TArray<FString>& Args, UWorld* World)
{
	int32 ConnectionCount = 99;
	if (Args.Num() > 0)
	{
		LexFromString(ConnectionCount, *Args[0]);
	}

	// Search for server game net driver. Do it this way so we can cheat in PIE
	UNetDriver* BestNetDriver = nullptr;
	for (TObjectIterator<UNetDriver> NetDriverIt; NetDriverIt; ++NetDriverIt)
	{
		if (NetDriverIt->NetDriverName == NAME_GameNetDriver && NetDriverIt->IsServer())
		{
			BestNetDriver = *NetDriverIt;
			break;
		}
	}

	if (!BestNetDriver)
	{
		return;
	}

	AActor* DefaultViewTarget = nullptr;
	APlayerController* PC = nullptr;
	for (auto Iterator = BestNetDriver->GetWorld()->GetPlayerControllerIterator(); Iterator; ++Iterator)
	{
		PC = Iterator->Get();
		if (PC)
		{
			DefaultViewTarget = PC->GetViewTarget();
			break;
		}
	}
	

	UE_LOG(LogNet, Display, TEXT("Adding %d Simulated Connections..."), ConnectionCount);
	while(ConnectionCount-- > 0)
	{
		USimulatedClientNetConnection* Connection = NewObject<USimulatedClientNetConnection>();
		Connection->InitConnection( BestNetDriver, USOCK_Open, BestNetDriver->GetWorld()->URL, 1000000 );
		Connection->InitSendBuffer();
		BestNetDriver->AddClientConnection( Connection );
		Connection->HandleClientPlayer(PC, Connection);
		Connection->SetClientWorldPackageName(BestNetDriver->GetWorldPackage()->GetFName());
	}	
}

FAutoConsoleCommandWithWorldAndArgs AddimulatedConnectionsCmd(TEXT("net.SimulateConnections"), TEXT("Starts a Simulated Net Driver"),	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(AddSimulatedNetConnections) );

// ----------------------------------------------------------------


static void	PrintActorReportFunc(const TArray<FString>& Args, UWorld* InWorld)
{
	// Search for server game net driver. Do it this way so we can cheat in PIE
	UNetDriver* BestNetDriver = nullptr;
	for (TObjectIterator<UNetDriver> NetDriverIt; NetDriverIt; ++NetDriverIt)
	{
		if (NetDriverIt->NetDriverName == NAME_GameNetDriver && NetDriverIt->IsServer())
		{
			BestNetDriver = *NetDriverIt;
			break;
		}
	}

	int32 TotalCount = 0;
	
	TMap<UClass*, int32> ClassCount;
	TMap<UClass*, int32> ActualClassCount;
	TMap<ENetDormancy, int32> DormancyCount;
	FBox BoundingBox;

	TMap<AActor*, int32> RawActorPtrMap;
	TMap<TWeakObjectPtr<AActor>, int32> WeakPtrMap;
	TMap<FObjectKey, int32> ObjKeyMap;

	UWorld* World = BestNetDriver ? BestNetDriver->GetWorld() : InWorld;
	if (!World)
		return;

	
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor->GetIsReplicated() == false)
		{
			continue;
		}

		TotalCount++;
		DormancyCount.FindOrAdd(Actor->NetDormancy)++;

		BoundingBox += Actor->GetActorLocation();

		UClass* CurrentClass = Actor->GetClass();

		ActualClassCount.FindOrAdd(CurrentClass)++;

		while(CurrentClass)
		{
			ClassCount.FindOrAdd(CurrentClass)++;
			CurrentClass = CurrentClass->GetSuperClass();
		}

		RawActorPtrMap.Add(Actor) = FMath::Rand();
		WeakPtrMap.Add(Actor) = FMath::Rand();
		ObjKeyMap.Add(FObjectKey(Actor)) = FMath::Rand();
	}

	ClassCount.ValueSort(TGreater<int32>());
	ActualClassCount.ValueSort(TGreater<int32>());

	UE_LOG(LogNet, Display, TEXT("Class Count (includes inheritance)"));
	for (auto MapIt : ClassCount)
	{
		UE_LOG(LogNet, Display, TEXT("%s - %d"), *GetNameSafe(MapIt.Key), MapIt.Value);
	}


	UE_LOG(LogNet, Display, TEXT(""));
	UE_LOG(LogNet, Display, TEXT("Class Count (actual clases)"));
	for (auto MapIt : ActualClassCount)
	{
		UE_LOG(LogNet, Display, TEXT("%s - %d"), *GetNameSafe(MapIt.Key), MapIt.Value);
	}

	UE_LOG(LogNet, Display, TEXT(""));
	UE_LOG(LogNet, Display, TEXT("Complete Bounding Box: %s"), *BoundingBox.ToString());
	UE_LOG(LogNet, Display, TEXT("                 Size: %s"), *BoundingBox.GetSize().ToString());

	UE_LOG(LogNet, Display, TEXT(""));

	for (auto MapIt : DormancyCount)
	{
		UE_LOG(LogNet, Display, TEXT("%s - %d"), *UEnum::GetValueAsString(TEXT("/Script/Engine.ENetDormancy"), MapIt.Key), MapIt.Value);
	}

	UE_LOG(LogNet, Display, TEXT(""));
	UE_LOG(LogNet, Display, TEXT("Total Replicated Actor Count: %d"), TotalCount);


	UE_LOG(LogNet, Display, TEXT(""));
	UE_LOG(LogNet, Display, TEXT("Raw Actor Map: "));
	RawActorPtrMap.Dump(*GLog);

	UE_LOG(LogNet, Display, TEXT(""));
	UE_LOG(LogNet, Display, TEXT("Weak Ptr Map: "));
	WeakPtrMap.Dump(*GLog);

	UE_LOG(LogNet, Display, TEXT(""));
	UE_LOG(LogNet, Display, TEXT("ObjectKey Map: "));
	ObjKeyMap.Dump(*GLog);
}

FAutoConsoleCommandWithWorldAndArgs PrintActorReportCmd(TEXT("net.ActorReport"), TEXT(""),	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(PrintActorReportFunc) );

/*-----------------------------------------------------------------------------
	FChannelRecordImpl
-----------------------------------------------------------------------------*/

namespace FChannelRecordImpl
{

void PushPacketId(FWrittenChannelsRecord& WrittenChannelsRecord, int32 PacketId)
{
	if (PacketId != WrittenChannelsRecord.LastPacketId)
	{
		FWrittenChannelsRecord::FChannelRecordEntry PacketEntry = { uint32(PacketId), 1u };
		WrittenChannelsRecord.ChannelRecord.Enqueue(PacketEntry);
		WrittenChannelsRecord.LastPacketId = PacketId;
	}
}

void PushChannelRecord(FWrittenChannelsRecord& WrittenChannelsRecord, int32 PacketId, int32 ChannelIndex)
{
	PushPacketId(WrittenChannelsRecord, PacketId);

	FWrittenChannelsRecord::FChannelRecordEntry ChannelEntry = { uint32(ChannelIndex), 0u };
	WrittenChannelsRecord.ChannelRecord.Enqueue(ChannelEntry);
}

SIZE_T CountBytes(FWrittenChannelsRecord& WrittenChannelsRecord)
{
	return WrittenChannelsRecord.ChannelRecord.AllocatedCapacity() * sizeof(FWrittenChannelsRecord::FChannelRecordEntry);
}

template<class Functor>
void ConsumeChannelRecordsForPacket(FWrittenChannelsRecord& WrittenChannelsRecord, int32 PacketId, Functor&& Func)
{
	const int32 ExpectedSeq = PacketId;
	uint32 PreviousChannelIndex = uint32(-1);

	FWrittenChannelsRecord::FChannelRecordEntryQueue& Record = WrittenChannelsRecord.ChannelRecord;

	// We should ALWAYS have data when we get here
	FWrittenChannelsRecord::FChannelRecordEntry PacketEntry = Record.Peek();
	Record.Pop();

	// Verify that we got the expected packetId
	check(PacketEntry.IsSequence == 1u && PacketEntry.Value == (uint32)PacketId);

	while (!Record.IsEmpty() && Record.PeekNoCheck().IsSequence == 0u)
	{
		const FWrittenChannelsRecord::FChannelRecordEntry Entry = Record.PeekNoCheck();
		Record.PopNoCheck();

		const uint32 ChannelIndex = Entry.Value;

		// Only process channel once per packet
		if (ChannelIndex != PreviousChannelIndex)
		{
			Func(PacketId, ChannelIndex);
			PreviousChannelIndex = ChannelIndex;
		}
	}
}

template<class Functor>
void ConsumeAllChannelRecords(FWrittenChannelsRecord& WrittenChannelsRecord, Functor&& Func)
{
	// Consume all records
	uint32 PreviousChannelIndex = uint32(-1);
	FWrittenChannelsRecord::FChannelRecordEntryQueue& Record = WrittenChannelsRecord.ChannelRecord;

	while (!Record.IsEmpty())
	{
		const FWrittenChannelsRecord::FChannelRecordEntry Entry = Record.PeekNoCheck();
		Record.PopNoCheck();

		const uint32 ChannelIndex = Entry.Value;

		// if the channel writes data multiple non-consecutive times between ticks, the func will be invoked multiple times which should not be an issue.
		if (Entry.IsSequence == 0u && ChannelIndex != PreviousChannelIndex)
		{
			Func(ChannelIndex);
			PreviousChannelIndex = ChannelIndex;
		}
	}
}

}


  