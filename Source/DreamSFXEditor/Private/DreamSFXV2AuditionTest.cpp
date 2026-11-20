#include "SDreamSFXSoundEditor.h"
#include "DreamSFXGraphAsset.h"
#include "DreamSFXPreviewSubsystem.h"
#include "Components/AudioComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamSFXV2AuditionTransport, "DreamSFX.Editor.V2AuditionTransport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamSFXV2AuditionTransport::RunTest(const FString&)
{
    FString Factory;
    FParse::Value(FCommandLine::Get(), TEXT("DreamSFXV2Fixtures="), Factory);
    auto* Preview = GEditor ? GEditor->GetEditorSubsystem<UDreamSFXPreviewSubsystem>() : nullptr;
    const UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestTrue(TEXT("Audition test requires the V2 library and an editor audio device"),
        !Factory.IsEmpty() && Preview && World && World->GetAudioDeviceRaw())) return false;

    const auto Panel = SNew(SDreamSFXSoundEditor).LibraryRoot(Factory);
    const float PreviousGain = Preview->GetPreviewVolumeMultiplier();
    Preview->SetPreviewVolumeMultiplier(4.f);
    Panel->LoopPreview = true;
    const double Start = FPlatformTime::Seconds();
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FFunctionLatentCommand>(
        [this, Panel, Preview, PreviousGain, Start, Step = 0, OldComponent = TWeakObjectPtr<UAudioComponent>()]() mutable
        {
            const auto Finish = [&]()
            {
                Panel->Stop();
                TestNull(TEXT("Stop removes the preview component"), Preview->Component.Get());
                Preview->SetPreviewVolumeMultiplier(PreviousGain);
                return true;
            };
            const double Now = FPlatformTime::Seconds();
            if (Now - Start > 25.)
            {
                AddError(FString::Printf(TEXT("V2 audition timed out at step %d: %s"), Step, *Panel->Status));
                return Finish();
            }
            Panel->Tick(FGeometry(), Now, 0.f);
            if (Panel->IsPreparing()) return false;

            const auto SelectAndPlay = [&](const FString& Id)
            {
                TArray<TSharedPtr<FDreamSFXSoundTreeNode>> Nodes = Panel->Roots;
                while (!Nodes.IsEmpty())
                {
                    const auto Node = Nodes.Pop(EAllowShrinking::No);
                    if (Node->Id == Id)
                    {
                        Panel->Select(Node, ESelectInfo::OnMouseClick);
                        Panel->Play();
                        return true;
                    }
                    Nodes.Append(Node->Children);
                }
                AddError(TEXT("Cannot select V2 preview row: ") + Id);
                return false;
            };
            if (Step == 0)
            {
                if (!SelectAndPlay(TEXT("benchmark.sine_confirm_soft"))) return Finish();
                ++Step;
                return false;
            }

            UAudioComponent* Active = Preview->Component;
            if (!Active || !Active->IsPlaying() || !Panel->IsWaveformCurrent()) return false;
            const auto* Data = FindObject<UDreamSFXGraphAsset>(Active->GetSound(), TEXT("DreamSFXGraph"));
            const FName ExpectedId(Step == 1 ? TEXT("benchmark.sine_confirm_soft") : TEXT("benchmark.noise_steady"));
            TestTrue(TEXT("The audio component plays the selected V2 GraphPlayer"),
                Data && Data->GetGraph() && Data->GetGraph()->Id == ExpectedId);
            TestEqual(TEXT("Monitor multiplier reaches the actual component"), Active->VolumeMultiplier, 4.f);
            TestTrue(TEXT("The text graph produces an audible waveform"), Panel->WaveData->RMS > 0.);
            if (Step == 1)
            {
                OldComponent = Active;
                if (!SelectAndPlay(TEXT("benchmark.noise_steady"))) return Finish();
                TestTrue(TEXT("Selecting the next sound stops the previous component"),
                    !OldComponent.IsValid() || !OldComponent->IsPlaying());
                ++Step;
                return false;
            }
            TestTrue(TEXT("The next sound owns a different component"), Active != OldComponent.Get());
            AddInfo(TEXT("DSFX_V2_AUDITION selected=2 replacements=1 monitor=4 waveform=1"));
            return Finish();
        }));
    return true;
}
#endif
