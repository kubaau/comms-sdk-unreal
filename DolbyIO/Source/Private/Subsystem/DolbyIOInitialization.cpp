// Copyright 2023 Dolby Laboratories

#include "DolbyIO.h"

#include "DolbyIODevices.h"
#include "Utils/DolbyIOBroadcastEvent.h"
#include "Utils/DolbyIOConversions.h"
#include "Utils/DolbyIOErrorHandler.h"
#include "Utils/DolbyIOLogging.h"
#include "Video/DolbyIOVideoFrameHandler.h"
#include "Video/DolbyIOVideoSink.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/Paths.h"
#include "TimerManager.h"

using namespace dolbyio::comms;
using namespace dolbyio::comms::plugin;
using namespace DolbyIO;

void UDolbyIOSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	ConferenceStatus = conference_status::destroyed;

	VideoSinks.Emplace(LocalCameraTrackID, std::make_shared<FVideoSink>(LocalCameraTrackID));
	VideoSinks.Emplace(LocalScreenshareTrackID, std::make_shared<FVideoSink>(LocalScreenshareTrackID));
	LocalCameraFrameHandler = std::make_shared<FVideoFrameHandler>(VideoSinks[LocalCameraTrackID]);
	LocalScreenshareFrameHandler = std::make_shared<FVideoFrameHandler>(VideoSinks[LocalScreenshareTrackID]);

	FTimerManager& TimerManager = GetGameInstance()->GetTimerManager();
	TimerManager.SetTimer(LocationTimerHandle, this, &UDolbyIOSubsystem::SetLocationUsingFirstPlayer, 0.1, true);
	TimerManager.SetTimer(RotationTimerHandle, this, &UDolbyIOSubsystem::SetRotationUsingFirstPlayer, 0.01, true);

	BroadcastEvent(OnTokenNeeded);
}

void UDolbyIOSubsystem::SetLogSettings(EDolbyIOLogLevel SdkLogLevel, EDolbyIOLogLevel MediaLogLevel,
                                       EDolbyIOLogLevel DvcLogLevel)
{
	const FString& LogDir = FPaths::ProjectLogDir();
	DLB_UE_LOG("Logs will be saved in directory %s", *LogDir);

	sdk::log_settings LogSettings;
	LogSettings.sdk_log_level = ToSdkLogLevel(SdkLogLevel);
	LogSettings.media_log_level = ToSdkLogLevel(MediaLogLevel);
	LogSettings.dvc_log_level = ToSdkLogLevel(DvcLogLevel);
	LogSettings.log_directory = ToStdString(LogDir);
	LogSettings.suppress_stdout_logs = true;
	sdk::set_log_settings(LogSettings);
}

void UDolbyIOSubsystem::SetToken(const FString& Token)
{
	if (!Sdk)
	{
		DLB_UE_LOG("Initializing with token: %s", *Token);
		AsyncTask(ENamedThreads::AnyThread, [this, Token] { Initialize(Token); });
	}
	else if (RefreshTokenCb)
	{
		DLB_UE_LOG("Refreshing token: %s", *Token);
		try
		{
			(*RefreshTokenCb)(ToStdString(Token));
		}
		catch (...)
		{
			DLB_ERROR_HANDLER.HandleError();
		}
		RefreshTokenCb.Reset(); // RefreshToken callback can be called only once
	}
}

void UDolbyIOSubsystem::Initialize(const FString& Token)
{
	try
	{
		Sdk = TSharedPtr<sdk>(sdk::create(ToStdString(Token),
		                                  [this](std::unique_ptr<refresh_token>&& RefreshCb)
		                                  {
			                                  DLB_UE_LOG("Refresh token requested");
			                                  RefreshTokenCb = TSharedPtr<refresh_token>(RefreshCb.release());
			                                  BroadcastEvent(OnTokenNeeded);
		                                  })
		                          .release());
	}
	catch (...)
	{
		DLB_ERROR_HANDLER.HandleError();
		return;
	}

	Sdk->register_component_version("unreal-sdk", "1.2.0-beta.1")
	    .then(
	        [this](sdk::component_data)
	        {
		        return Sdk->conference().add_event_handler([this](const conference_status_updated& Event)
		                                                   { UpdateStatus(Event.status); });
	        })
	    .then(
	        [this](event_handler_id)
	        {
		        return Sdk->conference().add_event_handler(
		            [this](const remote_participant_added& Event)
		            {
			            if (!Event.participant.status)
			            {
				            return;
			            }
			            const FDolbyIOParticipantInfo Info = ToFDolbyIOParticipantInfo(Event.participant);
			            DLB_UE_LOG("Participant status added: UserID=%s Name=%s ExternalID=%s Status=%s", *Info.UserID,
			                       *Info.Name, *Info.ExternalID, *ToString(*Event.participant.status));
			            {
				            FScopeLock Lock{&RemoteParticipantsLock};
				            RemoteParticipants.Emplace(Info.UserID, Info);
			            }

			            BroadcastEvent(OnParticipantAdded, Info.Status, Info);

			            if (TArray<FDolbyIOVideoTrack>* ParticipantTracks = BufferedVideoTracks.Find(Info.UserID))
			            {
				            for (const FDolbyIOVideoTrack& VideoTrack : *ParticipantTracks)
				            {
					            DLB_UE_LOG("Video track added: TrackID=%s ParticipantID=%s", *VideoTrack.TrackID,
					                       *VideoTrack.ParticipantID);
					            BroadcastEvent(OnVideoTrackAdded, VideoTrack);
				            }
				            BufferedVideoTracks.Remove(Info.UserID);
			            }
		            });
	        })
	    .then(
	        [this](event_handler_id)
	        {
		        return Sdk->conference().add_event_handler(
		            [this](const remote_participant_updated& Event)
		            {
			            if (!Event.participant.status)
			            {
				            return;
			            }
			            const FDolbyIOParticipantInfo Info = ToFDolbyIOParticipantInfo(Event.participant);
			            DLB_UE_LOG("Participant status updated: UserID=%s Name=%s ExternalID=%s Status=%s",
			                       *Info.UserID, *Info.Name, *Info.ExternalID, *ToString(*Event.participant.status));
			            {
				            FScopeLock Lock{&RemoteParticipantsLock};
				            RemoteParticipants.FindOrAdd(Info.UserID) = Info;
			            }

			            BroadcastEvent(OnParticipantUpdated, Info.Status, Info);
		            });
	        })
	    .then(
	        [this](event_handler_id)
	        {
		        return Sdk->conference().add_event_handler(
		            [this](const active_speaker_changed& Event)
		            {
			            TArray<FString> ActiveSpeakers;
			            for (const std::string& Speaker : Event.active_speakers)
			            {
				            ActiveSpeakers.Add(ToFString(Speaker));
			            }
			            BroadcastEvent(OnActiveSpeakersChanged, ActiveSpeakers);
		            });
	        })
	    .then(
	        [this](event_handler_id)
	        {
		        return Sdk->conference().add_event_handler(
		            [this](const audio_levels& Event)
		            {
			            TArray<FString> ActiveSpeakers;
			            TArray<float> AudioLevels;
			            for (const audio_level& Level : Event.levels)
			            {
				            ActiveSpeakers.Add(ToFString(Level.participant_id));
				            AudioLevels.Add(Level.level);
			            }
			            BroadcastEvent(OnAudioLevelsChanged, ActiveSpeakers, AudioLevels);
		            });
	        })
	    .then(
	        [this](event_handler_id)
	        {
		        return Sdk->conference().add_event_handler(
		            [this](const remote_video_track_added& Event)
		            {
			            const FDolbyIOVideoTrack VideoTrack = ToFDolbyIOVideoTrack(Event.track);

			            VideoSinks.Emplace(VideoTrack.TrackID, std::make_shared<FVideoSink>(VideoTrack.TrackID));
			            Sdk->video()
			                .remote()
			                .set_video_sink(Event.track, VideoSinks[VideoTrack.TrackID])
			                .on_error(DLB_ERROR_HANDLER);

			            FScopeLock Lock{&RemoteParticipantsLock};
			            if (RemoteParticipants.Contains(VideoTrack.ParticipantID))
			            {
				            DLB_UE_LOG("Video track added: TrackID=%s ParticipantID=%s", *VideoTrack.TrackID,
				                       *VideoTrack.ParticipantID);
				            BroadcastEvent(OnVideoTrackAdded, VideoTrack);
			            }
			            else
			            {
				            BufferedVideoTracks.FindOrAdd(VideoTrack.ParticipantID).Add(VideoTrack);
			            }
		            });
	        })
	    .then(
	        [this](event_handler_id)
	        {
		        return Sdk->conference().add_event_handler(
		            [this](const remote_video_track_removed& Event)
		            {
			            const FDolbyIOVideoTrack VideoTrack = ToFDolbyIOVideoTrack(Event.track);
			            DLB_UE_LOG("Video track removed: TrackID=%s ParticipantID=%s", *VideoTrack.TrackID,
			                       *VideoTrack.ParticipantID);

			            VideoSinks[VideoTrack.TrackID]->UnbindAllMaterials();
			            VideoSinks.Remove(VideoTrack.TrackID);
			            BroadcastEvent(OnVideoTrackRemoved, VideoTrack);
		            });
	        })
	    .then(
	        [this](event_handler_id)
	        {
		        Devices = MakeShared<FDevices>(*this, Sdk->device_management());
		        return Devices->RegisterDeviceEventHandlers();
	        })
	    .then([this](event_handler_id)
#if PLATFORM_WINDOWS
	          { return Sdk->device_management().set_default_audio_device_policy(default_audio_device_policy::output); })
	    .then([this]
#endif
#if PLATFORM_WINDOWS | PLATFORM_MAC
	          // keep this comment here to avoid wrong clang-formatting
	          { return video_processor::create(*Sdk); })
	    .then([this](std::shared_ptr<video_processor> VideoProcessorPtr)
	          { VideoProcessor = std::move(VideoProcessorPtr); })
	    .then(
	        [this]
#endif
	        {
		        using namespace dolbyio::comms::utils;
		        vfs_event::add_event_handler(*Sdk,
		                                     [this](const vfs_event& Event)
		                                     {
			                                     for (const auto& TrackMapItem : Event.new_enabled)
			                                     {
				                                     const FDolbyIOVideoTrack VideoTrack =
				                                         ToFDolbyIOVideoTrack(TrackMapItem);
				                                     DLB_UE_LOG("Video track ID %s for participant ID %s enabled",
				                                                *VideoTrack.TrackID, *VideoTrack.ParticipantID);
				                                     BroadcastEvent(OnVideoTrackEnabled, VideoTrack);
			                                     }
			                                     for (const auto& TrackMapItem : Event.new_disabled)
			                                     {
				                                     const FDolbyIOVideoTrack VideoTrack =
				                                         ToFDolbyIOVideoTrack(TrackMapItem);
				                                     DLB_UE_LOG("Video track ID %s for participant ID %s disabled",
				                                                *VideoTrack.TrackID, *VideoTrack.ParticipantID);
				                                     BroadcastEvent(OnVideoTrackDisabled, VideoTrack);
			                                     }
		                                     });

		        DLB_UE_LOG("Initialized");
		        BroadcastEvent(OnInitialized);
	        })
	    .on_error(DLB_ERROR_HANDLER);
}

void UDolbyIOObserver::InitializeComponent()
{
	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GameInstance = World->GetGameInstance())
		{
			if (UDolbyIOSubsystem* DolbyIOSubsystem = GameInstance->GetSubsystem<UDolbyIOSubsystem>())
			{
#define DLB_BIND(Event) DolbyIOSubsystem->Event.AddDynamic(this, &UDolbyIOObserver::Fwd##Event);
				DLB_BIND(OnTokenNeeded);
				DLB_BIND(OnInitialized);
				DLB_BIND(OnConnected);
				DLB_BIND(OnDisconnected);
				DLB_BIND(OnParticipantAdded);
				DLB_BIND(OnParticipantUpdated);
				DLB_BIND(OnVideoTrackAdded);
				DLB_BIND(OnVideoTrackRemoved);
				DLB_BIND(OnVideoTrackEnabled);
				DLB_BIND(OnVideoTrackDisabled);
				DLB_BIND(OnVideoEnabled);
				DLB_BIND(OnVideoDisabled);
				DLB_BIND(OnScreenshareStarted);
				DLB_BIND(OnScreenshareStopped);
				DLB_BIND(OnActiveSpeakersChanged);
				DLB_BIND(OnAudioLevelsChanged);
				DLB_BIND(OnScreenshareSourcesReceived);
				DLB_BIND(OnAudioInputDevicesReceived);
				DLB_BIND(OnAudioOutputDevicesReceived)
				DLB_BIND(OnCurrentAudioInputDeviceReceived);
				DLB_BIND(OnCurrentAudioOutputDeviceReceived);
				DLB_BIND(OnVideoDevicesReceived);
				DLB_BIND(OnCurrentAudioInputDeviceChanged);
				DLB_BIND(OnCurrentAudioOutputDeviceChanged);
				FwdOnTokenNeeded();
			}
		}
	}
}