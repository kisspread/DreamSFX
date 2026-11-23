#include "DreamSFXGraphInstance.h"
#include "DreamSFXSoundDocument.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "MetasoundSource.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Sound/SoundGenerator.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace DreamSFX::V2
{
    namespace
    {
        constexpr int32 FixtureRate = 48000;
        constexpr int32 FixtureCallback = 256;

        class FFixtureRecorder
        {
        public:
            FFixtureRecorder(FAutomationTestBase& InTest, FString InFolder)
                : Test(InTest), Folder(MoveTemp(InFolder)) {}

            bool RecordGraph(const FString& Id, const FGraphPtr& Graph, UMetaSoundSource& Source)
            {
                FString Error;
                FGraphInstance Instance;
                if (!Instance.Prepare(Graph, FixtureRate, Error))
                {
                    Test.AddError(Id + TEXT(": ") + Error);
                    return false;
                }
                const int32 UpperBound = int32(Instance.GetLengthFrames());
                TArray<float> Direct;
                while (!Instance.IsFinished() && Direct.Num() <= UpperBound * 2)
                {
                    float Buffer[FixtureCallback * 2];
                    const int32 Copied = Instance.Render(Buffer, FixtureCallback);
                    if (Copied <= 0) break;
                    Direct.Append(Buffer, Copied * 2);
                }
                if (!Test.TestTrue(Id + TEXT(" core naturally finished"),
                    Instance.IsFinished() && !Direct.IsEmpty() && Direct.Num() <= UpperBound * 2)) return false;

                Source.InitResources();
                FSoundGeneratorInitParams Params;
                Params.AudioDeviceID = 0;
                Params.AudioComponentId = NextInstance;
                Params.InstanceID = NextInstance++;
                Params.SampleRate = FixtureRate;
                Params.NumChannels = 2;
                Params.NumFramesPerCallback = FixtureCallback;
                Params.AudioMixerNumOutputFrames = FixtureCallback;
                Params.GraphName = TEXT("DreamSFXV2.") + Id;
                auto Generator = Source.CreateSoundGenerator(Params, TArray<FAudioParameter>());
                if (!Test.TestTrue(Id + TEXT(" built a real MetaSound generator"), Generator.IsValid())) return false;
                Generator->OnBeginGenerate();
                ON_SCOPE_EXIT { Generator->OnEndGenerate(); Source.OnEndGenerate(Generator); };

                TArray<float> Capture;
                for (int32 At = 0; At < UpperBound + FixtureCallback * 4 && !Generator->IsFinished(); At += FixtureCallback)
                {
                    float Buffer[FixtureCallback * 2] = {};
                    Generator->GetNextBuffer(Buffer, UE_ARRAY_COUNT(Buffer), true);
                    Capture.Append(Buffer, UE_ARRAY_COUNT(Buffer));
                }
                if (!Test.TestTrue(Id + TEXT(" generator naturally finished"),
                    Generator->IsFinished() && Capture.Num() >= Direct.Num())) return false;
                if (!Test.TestTrue(Id + TEXT(" MetaSound matches core at offset zero"),
                    FMemory::Memcmp(Direct.GetData(), Capture.GetData(), Direct.Num() * sizeof(float)) == 0)) return false;

                float Peak = 0;
                for (int32 Index = 0; Index < Capture.Num(); ++Index)
                {
                    const float Sample = Capture[Index];
                    if (!FMath::IsFinite(Sample) || (Index >= Direct.Num() && Sample != 0.f))
                    {
                        Test.AddError(Id + TEXT(" has invalid PCM or nonzero transport padding"));
                        return false;
                    }
                    Peak = FMath::Max(Peak, FMath::Abs(Sample));
                }
                if (!Test.TestTrue(Id + TEXT(" finite non-clipping signal"), Peak > 1.e-5f && Peak < 1.f)) return false;

                const FString Path = Folder / (Id + TEXT(".metasound.f32"));
                const auto Bytes = MakeArrayView(reinterpret_cast<const uint8*>(Capture.GetData()),
                    Capture.Num() * sizeof(float));
                if (!Test.TestTrue(Id + TEXT(" saved unscaled float PCM"),
                    !FPaths::FileExists(Path) && FFileHelper::SaveArrayToFile(Bytes, *Path))) return false;

                Test.AddInfo(FString::Printf(TEXT("DSFX_V2_CASE %s natural=%d captured=%d peak=%.9f canonical=0 endOfScore=%lld"),
                    *Id, Direct.Num() / 2, Capture.Num() / 2, Peak,
                    FMath::RoundToInt64(Graph->DurationSeconds * FixtureRate)));
                return true;
            }

        private:
            FAutomationTestBase& Test;
            FString Folder;
            uint64 NextInstance = 986500;
        };
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringCapture, "DreamSFX.V2.AuthoringCapture",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FAuthoringCapture::RunTest(const FString&)
    {
        FString Root, Out;
        if (!FParse::Value(FCommandLine::Get(), TEXT("DreamSFXAuthoringRoot="), Root)) return true;
        if (!FParse::Value(FCommandLine::Get(), TEXT("DreamSFXAuthoringOut="), Out) ||
            !IFileManager::Get().MakeDirectory(*Out, true)) return false;
        auto Library = MakeShared<FDreamSFXLibrarySnapshot, ESPMode::ThreadSafe>();
        if (!Library->Load(Root)) { AddError(Library->Error); return false; }
        auto* Async = IConsoleManager::Get().FindConsoleVariable(TEXT("au.MetaSound.EnableAsyncGeneratorBuilder"));
        if (!TestNotNull(TEXT("MetaSound generator setting"), Async)) return false;
        const int32 Old = Async->GetInt();
        Async->Set(0, ECVF_SetByCode);
        ON_SCOPE_EXIT { Async->Set(Old, ECVF_SetByCode); };
        FFixtureRecorder Recorder(*this, Out);
        for (const auto& Entry : Library->Entries)
        {
            FDreamSFXSoundDocument Doc;
            FString Error;
            if (!Doc.LoadFromSnapshot(Library, Entry.Id, Error)) { AddError(Entry.Id + TEXT(" ") + Error); continue; }
            const auto Package = GraphPackageName(FName(*Entry.Id));
            const auto Path = Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package);
            auto* Source = LoadObject<UMetaSoundSource>(nullptr, *Path);
            if (!TestNotNull(Entry.Id + TEXT(" cold saved asset"), Source)) continue;
            if (!Recorder.RecordGraph(Entry.Id, Doc.GetGraph(), *Source)) return false;
        }
        return !Library->Entries.IsEmpty() && !HasAnyErrors();
    }
}
#endif
