#include "DreamSFXGraphCompiler.h"
#include "DreamSFXGraphInstance.h"
#include "DreamSFXGraphAsset.h"
#include "DreamSFXGraphPreview.h"
#include "DreamSFXSoundDocument.h"
#include "DreamSFXGraphArchive.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "MetasoundSource.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundGenerator.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace DreamSFX::V2
{
    namespace
    {
        constexpr int32 FixtureRate = 48000;
        constexpr int32 FixtureCallback = 256;

        bool SamePcm(TConstArrayView<float> Left, TConstArrayView<float> Right)
        {
            return Left.Num() == Right.Num() && FMemory::Memcmp(Left.GetData(), Right.GetData(), Left.Num() * sizeof(float)) == 0;
        }

        struct FObservedEvent
        {
            EAction Action;
            int64 Frame;
            FPortRef Target;
            FName NoteId;
            int32 Midi = 0;
            float Value = 0;
            bool operator==(const FObservedEvent&) const = default;
        };

        TArray<FObservedEvent> CollectVoiceAndCutoff(const FGraphIR& Graph)
        {
            TArray<FObservedEvent> Events;
            for (const FEvent& Event : Graph.Timeline)
            {
                FObservedEvent Item{Event.Action, FMath::RoundToInt64(Event.AtSeconds * FixtureRate), Event.To};
                if (const auto* Note = Event.Payload.TryGet<FNoteOn>())
                {
                    Item.NoteId = Note->Id;
                    Item.Midi = Note->Midi;
                    Item.Value = Note->Velocity;
                }
                else if (const auto* Off = Event.Payload.TryGet<FNoteOff>()) Item.NoteId = Off->Id;
                else if (const auto* Sustain = Event.Payload.TryGet<FSustain>()) Item.Value = Sustain->bDown;
                else if (const auto* Fire = Event.Payload.TryGet<FFire>()) Item.Value = Fire->Velocity;
                else if (Event.To.Port == TEXT("cutoffHz")) Item.Value = Event.Value;
                else continue;
                Events.Add(Item);
            }
            return Events;
        }

        const TCHAR* ActionName(EAction Action)
        {
            switch (Action)
            {
            case EAction::NoteOn: return TEXT("noteOn");
            case EAction::NoteOff: return TEXT("noteOff");
            case EAction::Sustain: return TEXT("sustain");
            case EAction::Fire: return TEXT("fire");
            default: return TEXT("cutoff");
            }
        }

        struct FRecordedFixture
        {
            FGraphPtr Graph;
            TArray<FObservedEvent> Events;
            TArray<float> Capture;
            int32 NaturalFrames = 0;
        };

        class FFixtureRecorder
        {
        public:
            FFixtureRecorder(FAutomationTestBase& InTest, FString InFolder) : Test(InTest), Folder(MoveTemp(InFolder)) {}

            bool Record(const FString& Id, const FString& Text, FRecordedFixture& Result, bool bCanonical,
                UMetaSoundSource* PersistedSource = nullptr)
            {
                const auto Compiled = FGraphCompiler::Compile(Text);
                for (const auto& Error : Compiled.Errors) Test.AddError(Id + TEXT(": ") + Error.Code + TEXT(" ") + Error.Pointer + TEXT(" ") + Error.Message);
                if (!Test.TestTrue(Id + TEXT(" compiles as V2"), Compiled.Graph.IsValid())) return false;
                return RecordGraph(Id, Compiled.Graph, Result, bCanonical, PersistedSource);
            }

            bool RecordGraph(const FString& Id, FGraphPtr Prepared, FRecordedFixture& Result, bool bCanonical,
                UMetaSoundSource* PersistedSource)
            {
                Result.Graph = MoveTemp(Prepared);
                if (Id == TEXT("blast-full-opening"))
                {
                    if (!Test.TestEqual(TEXT("Opening nodes"), Result.Graph->Nodes.Num(), 494)) return false;
                }
                else if (!PersistedSource) Result.Events = CollectVoiceAndCutoff(*Result.Graph);

                FString Error;
                FGraphInstance Instance;
                if (!Instance.Prepare(Result.Graph, FixtureRate, Error))
                {
                    Test.AddError(Id + TEXT(": ") + Error);
                    return false;
                }
                const int32 UpperBound = int32(Instance.GetLengthFrames());
                auto RenderCore = [&](int32 Chunk)
                {
                    Instance.Restart();
                    TArray<float> Audio;
                    float Scratch[2048];
                    while (!Instance.IsFinished() && Audio.Num() <= UpperBound * 2)
                    {
                        const int32 Copied = Instance.Render(Scratch, Chunk);
                        if (Copied <= 0) break;
                        Audio.Append(Scratch, Copied * 2);
                    }
                    Test.TestTrue(Id + TEXT(" core naturally finished"), Instance.IsFinished() && !Audio.IsEmpty() && Audio.Num() <= UpperBound * 2);
                    return Audio;
                };
                const auto Direct = RenderCore(FixtureCallback);
                Result.NaturalFrames = Direct.Num() / 2;
                if (!Result.NaturalFrames) return false;
                if (bCanonical)
                {
                    for (int32 Chunk : {64, 1024})
                    {
                        if (!Test.TestTrue(Id + FString::Printf(TEXT(" PCM and completion match at %d frames"), Chunk), SamePcm(Direct, RenderCore(Chunk)))) return false;
                    }
                }

                UMetaSoundSource* Source = PersistedSource ? PersistedSource : BuildGraphPreview(Result.Graph, Error);
                if (!Source)
                {
                    Test.AddError(Id + TEXT(": ") + Error);
                    return false;
                }
                Source->InitResources();
                FSoundGeneratorInitParams Params;
                Params.AudioDeviceID = 0;
                Params.AudioComponentId = NextInstance;
                Params.InstanceID = NextInstance++;
                Params.SampleRate = FixtureRate;
                Params.NumChannels = 2;
                Params.NumFramesPerCallback = FixtureCallback;
                Params.AudioMixerNumOutputFrames = FixtureCallback;
                Params.GraphName = TEXT("DreamSFXV2.") + Id;
                auto Generator = Source->CreateSoundGenerator(Params, TArray<FAudioParameter>());
                if (!Test.TestTrue(Id + TEXT(" built a real MetaSound generator"), Generator.IsValid())) return false;
                Generator->OnBeginGenerate();
                ON_SCOPE_EXIT { Generator->OnEndGenerate(); Source->OnEndGenerate(Generator); };
                for (int32 At = 0; At < UpperBound + FixtureCallback * 4 && !Generator->IsFinished(); At += FixtureCallback)
                {
                    float Buffer[FixtureCallback * 2] = {};
                    Generator->GetNextBuffer(Buffer, UE_ARRAY_COUNT(Buffer), true);
                    Result.Capture.Append(Buffer, UE_ARRAY_COUNT(Buffer));
                }
                if (!Test.TestTrue(Id + TEXT(" generator naturally finished"), Generator->IsFinished() && Result.Capture.Num() >= Direct.Num())) return false;
                if (!Test.TestTrue(Id + TEXT(" MetaSound matches core at offset zero"), SamePcm(Direct, MakeArrayView(Result.Capture.GetData(), Direct.Num())))) return false;
                float Peak = 0;
                for (int32 Index = 0; Index < Result.Capture.Num(); ++Index)
                {
                    const float Sample = Result.Capture[Index];
                    if (!FMath::IsFinite(Sample) || (Index >= Direct.Num() && Sample != 0.f))
                    {
                        Test.AddError(Id + TEXT(" has invalid PCM or nonzero transport padding"));
                        return false;
                    }
                    Peak = FMath::Max(Peak, FMath::Abs(Sample));
                }
                if (!Test.TestTrue(Id + TEXT(" finite non-clipping signal"), Peak > 1.e-5f && Peak < 1.f)) return false;
                const FString Path = Folder / (Id + TEXT(".metasound.f32"));
                if (!Folder.IsEmpty() && !Test.TestTrue(Id + TEXT(" saved unscaled float PCM"), !FPaths::FileExists(Path) && FFileHelper::SaveArrayToFile(
                    MakeArrayView(reinterpret_cast<const uint8*>(Result.Capture.GetData()), Result.Capture.Num() * sizeof(float)), *Path))) return false;
                Test.AddInfo(FString::Printf(TEXT("DSFX_V2_CASE %s natural=%d captured=%d peak=%.9f canonical=%d endOfScore=%lld"),
                    *Id, Result.NaturalFrames, Result.Capture.Num() / 2, Peak, bCanonical,
                    FMath::RoundToInt64(Result.Graph->DurationSeconds * FixtureRate)));
                for (const auto& Event : Result.Events)
                {
                    Test.AddInfo(FString::Printf(TEXT("DSFX_V2_EVENT %s %s %lld %s %s %d %.9g"), *Id,
                        ActionName(Event.Action), Event.Frame, *Event.Target.Node.ToString(), *Event.NoteId.ToString(), Event.Midi, Event.Value));
                }
                return true;
            }

        private:
            FAutomationTestBase& Test;
            FString Folder;
            uint64 NextInstance = 986500;
        };

        bool ReadObject(const FString& Text, TSharedPtr<FJsonObject>& Object)
        {
            return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) && Object.IsValid();
        }

        FString WithEdit(const FString& Text, TFunctionRef<void(FJsonObject&)> Edit)
        {
            TSharedPtr<FJsonObject> Object;
            if (!ReadObject(Text, Object)) return {};
            Edit(*Object);
            FString Out;
            if (!FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<>::Create(&Out))) return {};
            return Out;
        }
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayerContract, "DreamSFX.V2.LayerContract",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FLayerContract::RunTest(const FString&)
    {
        const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamSFX"));
        if (!Plugin) return false;
        const auto Root = Plugin->GetBaseDir() / TEXT("Resources/Presets");
        for (const auto& Pair : {TPair<FString,int32>{TEXT("Magic/blastFull.dsfx.json"),70},
            {TEXT("Small Magic/Tornado/twisterFire.dsfx.json"),312}})
        {
            FString Original;
            if (!FFileHelper::LoadFileToString(Original, *(Root / Pair.Key))) return false;
            auto Default = FGraphCompiler::Compile(Original);
            auto Minus3 = FGraphCompiler::Compile(WithEdit(Original, [](FJsonObject& Object)
            {
                Object.GetObjectField(TEXT("layers"))->GetObjectField(TEXT("embers"))->SetNumberField(TEXT("gainDb"), -3.);
            }));
            if (!TestTrue(Pair.Key + TEXT(" compiles with one layer edit"), Default.Graph && Minus3.Graph)) return false;
            auto Comparison = *Minus3.Graph;
            int32 Changes = 0;
            for (int32 I = 0; I < Comparison.Nodes.Num(); ++I)
            {
                const auto& Base = Default.Graph->Nodes[I];
                auto& Node = Comparison.Nodes[I];
                if (!Node.Id.ToString().StartsWith(TEXT("embers.spark")) || !Node.Id.ToString().EndsWith(TEXT(".level"))) continue;
                const double Initial = Base.Parameters.FindChecked(TEXT("level")).Get<double>();
                const double Adjusted = Node.Parameters.FindChecked(TEXT("level")).Get<double>();
                if (!TestTrue(TEXT("Every independent ember uses the same dB trim"), FMath::Abs(Adjusted - Initial * FMath::Pow(10., -.15)) < 1.e-12)) return false;
                Node.Parameters = Base.Parameters;
                ++Changes;
            }
            TArray<uint8> Before, After;
            if (!TestEqual(TEXT("All expected ember gain fields"), Changes, Pair.Value) ||
                !TestTrue(TEXT("All other IR bytes, including event times and summing order, are identical"),
                    EncodeGraph(*Default.Graph, Before) && EncodeGraph(Comparison, After) && Before == After)) return false;
            AddInfo(FString::Printf(TEXT("DSFX_LAYER id=%s gains=%d events=%d connections=%d otherIRIdentical=1"),
                *Default.Graph->Id.ToString(), Changes, Default.Graph->Timeline.Num(), Default.Graph->Connections.Num()));
            const auto Invalid = FGraphCompiler::Compile(WithEdit(Original, [](FJsonObject& Object)
            {
                const auto Voice = Object.GetObjectField(TEXT("layers"))->GetObjectField(TEXT("embers"))->GetArrayField(TEXT("voices"))[0]->AsObject();
                Voice->GetObjectField(TEXT("args"))->SetStringField(TEXT("rate"), TEXT("fast"));
            }));
            if (!TestTrue(TEXT("Patch errors name the authored row and argument"), !Invalid.Graph && Invalid.Errors.Num() &&
                Invalid.Errors[0].Pointer == TEXT("/layers/embers/voices/0/args/rate"))) return false;
        }
        return true;
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
            FRecordedFixture Recorded;
            if (!Recorder.RecordGraph(Entry.Id, Doc.GetGraph(), Recorded, false, Source)) return false;
        }
        return !Library->Entries.IsEmpty() && !HasAnyErrors();
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFullBlastTest, "DreamSFX.V2.FullBlastSavedSource",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FFullBlastTest::RunTest(const FString&)
    {
        const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamSFX"));
        if (!TestTrue(TEXT("DreamSFX plugin is available"), Plugin.IsValid())) return false;
        const FString Path = Plugin->GetBaseDir() / TEXT("Resources/Presets/Magic/blastFull.dsfx.json");
        FString Text, Output;
        if (!TestTrue(TEXT("Full blast text is in the factory library"), FFileHelper::LoadFileToString(Text, *Path))) return false;
        if (FParse::Value(FCommandLine::Get(), TEXT("DreamSFXFullBlastCapture="), Output) &&
            !TestTrue(TEXT("Capture folder available"), IFileManager::Get().MakeDirectory(*Output, true))) return false;
        const FName Id(TEXT("small_magic.blastfull"));
        const FString Package = GraphPackageName(Id);
        const FString AssetPath = Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package);
        UMetaSoundSource* Source = LoadObject<UMetaSoundSource>(nullptr, *AssetPath);
        if (!TestNotNull(TEXT("Cold load the full blast MetaSound Source"), Source)) return false;
        const auto* Data = FindObject<UDreamSFXGraphAsset>(Source, TEXT("DreamSFXGraph"));
        if (!TestTrue(TEXT("Source owns the full V2 graph"), Data && Data->GetGraph() && Data->GetGraph()->Id == Id)) return false;
        auto* Async = IConsoleManager::Get().FindConsoleVariable(TEXT("au.MetaSound.EnableAsyncGeneratorBuilder"));
        if (!TestNotNull(TEXT("MetaSound generator setting"), Async)) return false;
        const int32 Previous = Async->GetInt();
        Async->Set(0, ECVF_SetByCode);
        ON_SCOPE_EXIT { Async->Set(Previous, ECVF_SetByCode); };
        FFixtureRecorder Recorder(*this, Output);
        FRecordedFixture Recorded;
        if (!Recorder.Record(TEXT("blastFull"), Text, Recorded, false, Source)) return false;
        if (!TestEqual(TEXT("Full stereo scene ends at 19 seconds"), Recorded.NaturalFrames, 912000)) return false;
        return TestTrue(TEXT("Last authored frame still carries the reference scene's wind bed"),
            FMath::Max(FMath::Abs(Recorded.Capture[1823998]), FMath::Abs(Recorded.Capture[1823999])) > .001f);
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSmallMagicScenesTest, "DreamSFX.V2.SmallMagicSavedScenes",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FSmallMagicScenesTest::RunTest(const FString&)
    {
        const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamSFX"));
        if (!Plugin) return false;
        FString Output, Selection;
        FParse::Value(FCommandLine::Get(), TEXT("DreamSFXSceneCapture="), Output);
        FParse::Value(FCommandLine::Get(), TEXT("DreamSFXSceneNames="), Selection);
        if (!Output.IsEmpty() && !IFileManager::Get().MakeDirectory(*Output, true)) return false;
        const FString Root = Plugin->GetBaseDir() / TEXT("Resources/Presets/Small Magic");
        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.dsfx.json"), true, false);
        TMap<FString, FString> Library;
        for (const FString& File : Files) Library.Add(FPaths::GetCleanFilename(File).LeftChop(10), File);
        TArray<FString> Scenes;
        if (Selection.IsEmpty()) Scenes.Add(TEXT("twisterFire"));
        else if (Selection == TEXT("all"))
        {
            Library.GenerateKeyArray(Scenes);
            Scenes.Sort();
        }
        else Selection.ParseIntoArray(Scenes, TEXT("+"), true);
        if (!TestTrue(TEXT("Selected at least one factory scene"), !Scenes.IsEmpty())) return false;
        auto* Async = IConsoleManager::Get().FindConsoleVariable(TEXT("au.MetaSound.EnableAsyncGeneratorBuilder"));
        if (!TestNotNull(TEXT("Synchronous MetaSound capture setting"), Async)) return false;
        const int32 Previous = Async->GetInt();
        Async->Set(0, ECVF_SetByCode);
        ON_SCOPE_EXIT { Async->Set(Previous, ECVF_SetByCode); };
        FFixtureRecorder Recorder(*this, Output);
        for (const FString& Scene : Scenes)
        {
            const FString* File = Library.Find(Scene);
            FString Text;
            if (!TestTrue(Scene + TEXT(" is present in the factory library"), File && FFileHelper::LoadFileToString(Text, **File))) return false;
            const FName Id(*(TEXT("small_magic.") + Scene.ToLower()));
            const FString Package = GraphPackageName(Id);
            const FString AssetPath = Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package);
            auto* Source = LoadObject<UMetaSoundSource>(nullptr, *AssetPath);
            if (!TestNotNull(Scene + TEXT(" saved MetaSound Source"), Source)) return false;
            const auto* Data = FindObject<UDreamSFXGraphAsset>(Source, TEXT("DreamSFXGraph"));
            if (!TestTrue(Scene + TEXT(" contains its V2 graph"), Data && Data->GetGraph() && Data->GetGraph()->Id == Id)) return false;
            FRecordedFixture Take;
            if (!Recorder.Record(Scene, Text, Take, false, Source)) return false;
            if (Scene == TEXT("twisterFire") && !TestEqual(TEXT("Complete fire tornado reference window"), Take.NaturalFrames, 720000)) return false;
        }
        return true;
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonoNoisePlaybackTest, "DreamSFX.V2.MonoNoisePlayback",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FMonoNoisePlaybackTest::RunTest(const FString&)
    {
        const auto Program = FGraphCompiler::Compile(TEXT(R"({
            "schema":"dreamsfx/2","id":"test.mono_noise","durationSeconds":0.01,
            "resources":[{"id":"noise","type":"buffer.white","version":1,"seed":123,"framesAt48k":4}],
            "nodes":[{"id":"source","type":"source.mono_noise_buffer","version":1,
                    "parameters":{"buffer":"noise","offsetSeconds":0,"playbackRate":1}},
                {"id":"pan","type":"mix.pan","version":1,"parameters":{"position":0}}],
            "connections":[{"from":{"node":"source","port":"audio"},"to":{"node":"pan","port":"input"},"order":0}],
            "timeline":[{"action":"trigger","atSeconds":0,"targets":[{"node":"source","port":"start"}]},
                {"action":"trigger","atSeconds":0.005,"targets":[{"node":"source","port":"stop"}]}],
            "output":{"node":"pan","port":"audio"}})"));
        FGraphInstance Voice;
        FString Error;
        if (!TestTrue(TEXT("Mono noise and equal-power panner prepare"), Program.Graph && Voice.Prepare(Program.Graph, FixtureRate, Error))) return false;
        float Audio[960] = {};
        Voice.Render(Audio, 480);
        uint32 Seed = 123;
        double Period[4];
        for (double& Sample : Period)
        {
            Seed = Seed * 1664525u + 1013904223u;
            Sample = (double(Seed) / 2147483648. - 1.) / FMath::Sqrt(2.);
        }
        double Worst = 0.;
        for (int32 Frame = 0; Frame < 480; ++Frame)
        {
            const double Expected = Frame < 240 ? Period[Frame % 4] : 0.;
            Worst = FMath::Max3(Worst, FMath::Abs(Audio[2 * Frame] - Expected), FMath::Abs(Audio[2 * Frame + 1] - Expected));
        }
        return TestTrue(TEXT("Mono source wraps at the buffer boundary, pans at -3 dB and stops on its gate"), Worst < 1.e-7);
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecayingNoiseRoomTest, "DreamSFX.V2.DecayingNoiseRoom",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FDecayingNoiseRoomTest::RunTest(const FString&)
    {
        const auto Program = FGraphCompiler::Compile(TEXT(R"({
            "schema":"dreamsfx/2","id":"test.decay_room","durationSeconds":0.012,
            "resources":[{"id":"pulse","type":"buffer.white","version":1,"seed":123,"framesAt48k":1},
                {"id":"ir","type":"buffer.white","version":1,"seed":456,"framesAt48k":256}],
            "nodes":[{"id":"source","type":"source.mono_noise_buffer","version":1,"parameters":{"buffer":"pulse","offsetSeconds":0,"playbackRate":1}},
                {"id":"room","type":"room.decaying_noise","version":1,"parameters":{"noise":"ir","attackSeconds":0.001,"decaySeconds":0.003}}],
            "connections":[{"from":{"node":"source","port":"audio"},"to":{"node":"room","port":"input"},"order":0}],
            "timeline":[{"action":"trigger","atSeconds":0,"targets":[{"node":"source","port":"start"}]},
                {"action":"trigger","atSeconds":0.00002083333333333333,"targets":[{"node":"source","port":"stop"}]}],
            "output":{"node":"room","port":"audio"}})"));
        FGraphInstance Voice;
        FString Error;
        if (!TestTrue(TEXT("Procedural stereo impulse prepares"), Program.Graph && Voice.Prepare(Program.Graph, FixtureRate, Error))) return false;
        float Audio[1152] = {};
        Voice.Render(Audio, 576);
        uint32 Seed = 456;
        double Response[2][256], Energy = 0.;
        for (auto& Channel : Response)
        {
            for (int32 I = 0; I < 256; ++I)
            {
                Seed = Seed * 1664525u + 1013904223u;
                Channel[I] = (double(Seed) / 2147483648. - 1.) * FMath::Min(1., I / 48.) * FMath::Exp(-I / 144.);
                Energy += Channel[I] * Channel[I];
            }
        }
        const double Pulse = double(123u * 1664525u + 1013904223u) / 2147483648. - 1.;
        const double Scale = Pulse * .00125 * (44100. / FixtureRate) / FMath::Sqrt(Energy / 512.);
        double Worst = 0.;
        for (int32 I = 0; I < 576; ++I)
            for (int32 Ch = 0; Ch < 2; ++Ch)
                Worst = FMath::Max(Worst, FMath::Abs(Audio[2 * I + Ch] - (I < 256 ? Scale * Response[Ch][I] : 0.)));
        return TestTrue(TEXT("Both convolved channels match the independent seeded impulse and its tail"), Worst < 1.e-7);
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSingleGrainFixture, "DreamSFX.V2.SingleGrainPlayback",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FSingleGrainFixture::RunTest(const FString&)
    {
        const auto Compiled = FGraphCompiler::Compile(TEXT(R"({
            "schema":"dreamsfx/2","id":"test.single_grain","durationSeconds":0.02,
            "resources":[{"id":"white","type":"buffer.white","version":1,"seed":123,"framesAt48k":8}],
            "nodes":[{"id":"grain","type":"source.grain_once","version":1,
                "parameters":{"buffer":"white","decaySeconds":0.001,"playbackRate":0.5}}],
            "connections":[],"output":{"node":"grain","port":"audio"},"timeline":[
                {"atSeconds":0.004005208333333333,"action":"trigger","timing":"continuous","targets":[{"node":"grain","port":"start"}]},
                {"atSeconds":0.012,"action":"trigger","targets":[{"node":"grain","port":"stop"}]}]})"));
        FString Error;
        FGraphInstance Voice;
        if (!TestTrue(TEXT("Named one-shot graph compiles and prepares"), Compiled.Graph && Voice.Prepare(Compiled.Graph, FixtureRate, Error))) return false;
        uint32 Seed = 123;
        double Expected[9] = {};
        for (int32 I = 0; I < 8; ++I)
        {
            Seed = Seed * 1664525u + 1013904223u;
            Expected[I] = (double(Seed) / 2147483648. - 1.) * FMath::Exp(-double(I) / 48.);
        }
        TArray<float> Audio;
        Audio.SetNumUninitialized(960 * 2);
        Voice.Render(Audio.GetData(), 960);
        double Worst = 0.;
        for (int32 Frame = 0; Frame < 960; ++Frame)
        {
            const double Read = (Frame - 192.25) * .5;
            const double Value = (Read < 0. || Read >= 8.) ? 0. :
                FMath::Lerp(Expected[int32(Read)], Expected[int32(Read) + 1], Read - int32(Read));
            Worst = FMath::Max3(Worst, FMath::Abs(Audio[2 * Frame] - Value), FMath::Abs(Audio[2 * Frame + 1] - Value));
        }
        AddInfo(FString::Printf(TEXT("DSFX_SINGLE_GRAIN onset=193 exactStart=192.25 length=16 maxDelta=%.12g"), Worst));
        return TestTrue(TEXT("One-shot interpolates the mono decayed resource and remains silent after its last sample"), Worst < 1.e-7);
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphFixtureTest, "DreamSFX.V2.NativeTextFixtures",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
    bool FGraphFixtureTest::RunTest(const FString&)
    {
        FString Input, Output, TimelinePath;
        FParse::Value(FCommandLine::Get(), TEXT("DreamSFXV2Fixtures="), Input);
        FParse::Value(FCommandLine::Get(), TEXT("DreamSFXV2CaptureDir="), Output);
        FParse::Value(FCommandLine::Get(), TEXT("DreamSFXV2Timeline="), TimelinePath);
        if (Input.IsEmpty() && Output.IsEmpty())
        {
            AddInfo(TEXT("Set -DreamSFXV2Fixtures and -DreamSFXV2CaptureDir to run the benchmark captures"));
            return true;
        }
        if (!TestTrue(TEXT("Use an existing fixture folder and a fresh capture folder"), FPaths::DirectoryExists(Input) &&
            !Output.IsEmpty() && !FPaths::DirectoryExists(Output) && IFileManager::Get().MakeDirectory(*Output, true))) return false;
        // These frozen examples can share a library with any number of production sounds.
        TArray<FString> Files;
        for (const TCHAR* Relative : {TEXT("Magic/blast-full-opening"), TEXT("MusicBox/musicbox-single"),
            TEXT("MusicBox/musicbox-small-clock"), TEXT("Nature/noise-steady"), TEXT("Nature/noise-wind-gust"),
            TEXT("OneShots/oneshot-coin"), TEXT("OneShots/oneshot-explosion"), TEXT("Piano/piano-small-clock"),
            TEXT("Piano/piano-sustain"), TEXT("Retro/retro-crickets"), TEXT("Retro/retro-square"), TEXT("UI/sine-confirm-soft")})
            Files.Add((Input / Relative) + TEXT(".dsfx.json"));

        FString HandWritten;
        TSharedPtr<FJsonObject> Explicit;
        const TArray<TSharedPtr<FJsonValue>>* ExplicitEvents = nullptr;
        if (!TestTrue(TEXT("Read independently authored explicit score timeline"), FFileHelper::LoadFileToString(HandWritten, *TimelinePath) &&
            ReadObject(HandWritten, Explicit) && Explicit->TryGetArrayField(TEXT("timeline"), ExplicitEvents) && ExplicitEvents->Num() == 32)) return false;
        auto* Async = IConsoleManager::Get().FindConsoleVariable(TEXT("au.MetaSound.EnableAsyncGeneratorBuilder"));
        if (!TestNotNull(TEXT("MetaSound generator setting"), Async)) return false;
        const int32 Previous = Async->GetInt();
        Async->Set(0, ECVF_SetByCode);
        ON_SCOPE_EXIT { Async->Set(Previous, ECVF_SetByCode); };

        FFixtureRecorder Recorder(*this, Output);
        for (const FString& File : Files)
        {
            FString Text;
            if (!TestTrue(File + TEXT(" read from disk"), FFileHelper::LoadFileToString(Text, *File))) return false;
            const FString Id = FPaths::GetCleanFilename(File).LeftChop(10); // .dsfx.json
            FRecordedFixture Primary;
            if (!Recorder.Record(Id, Text, Primary, true)) return false;
            if (Id.EndsWith(TEXT("-small-clock")))
            {
                const auto ExplicitText = WithEdit(Text, [&](FJsonObject& Object)
                {
                    Object.RemoveField(TEXT("sequence"));
                    Object.SetArrayField(TEXT("timeline"), *ExplicitEvents);
                });
                FRecordedFixture Other;
                if (!Recorder.Record(Id + TEXT("-explicit"), ExplicitText, Other, false)) return false;
                TestTrue(Id + TEXT(" score and hand-authored timeline schedule identical events"), Primary.Events == Other.Events);
                TestTrue(Id + TEXT(" score and hand-authored timeline yield identical MetaSound PCM and completion"),
                    Primary.NaturalFrames == Other.NaturalFrames && SamePcm(Primary.Capture, Other.Capture));
            }
            else if (Id == TEXT("piano-sustain"))
            {
                const auto WithoutPedal = WithEdit(Text, [](FJsonObject& Object)
                {
                    TArray<TSharedPtr<FJsonValue>> Events;
                    for (const auto& Item : Object.GetArrayField(TEXT("timeline")))
                        if (Item->AsObject()->GetStringField(TEXT("action")) != TEXT("sustain")) Events.Add(Item);
                    Object.SetArrayField(TEXT("timeline"), MoveTemp(Events));
                    Object.SetNumberField(TEXT("durationSeconds"), .1);
                });
                FRecordedFixture Other;
                if (!Recorder.Record(Id + TEXT("-unpedaled"), WithoutPedal, Other, false)) return false;
                TestTrue(TEXT("Pedal changes Piano output and lets it ring longer"), Primary.NaturalFrames > Other.NaturalFrames && !SamePcm(Primary.Capture, Other.Capture));
            }
            else if (Id == TEXT("retro-square"))
            {
                const auto WithExplicitDuty = WithEdit(Text, [](FJsonObject& Object)
                {
                    Object.GetArrayField(TEXT("nodes"))[0]->AsObject()->GetObjectField(TEXT("parameters"))->SetNumberField(TEXT("dutyCycle"), .5);
                });
                FRecordedFixture Other;
                if (!Recorder.Record(Id + TEXT("-explicit-duty"), WithExplicitDuty, Other, false)) return false;
                TestTrue(TEXT("Default Retro duty matches explicit 0.5"), SamePcm(Primary.Capture, Other.Capture));
            }
            else if (Id == TEXT("retro-crickets"))
            {
                const auto RawPulse = WithEdit(Text, [](FJsonObject& Object)
                {
                    Object.GetObjectField(TEXT("output"))->SetStringField(TEXT("node"), TEXT("pulse"));
                });
                FRecordedFixture Other;
                if (!Recorder.Record(Id + TEXT("-raw-pulse"), RawPulse, Other, false)) return false;
            }
            else if (Id == TEXT("noise-steady") || Id == TEXT("noise-wind-gust") || Id == TEXT("oneshot-explosion"))
            {
                FRecordedFixture Repeat;
                if (!Recorder.Record(Id + TEXT("-same-seed"), Text, Repeat, false)) return false;
                TestTrue(Id + TEXT(" fresh MetaSound generator with same seed is identical"), SamePcm(Primary.Capture, Repeat.Capture));
                const auto WithAnotherSeed = WithEdit(Text, [&](FJsonObject& Object)
                {
                    FJsonObject* SeedOwner = &Object;
                    if (Id == TEXT("oneshot-explosion")) SeedOwner = Object.GetArrayField(TEXT("nodes"))[0]->AsObject()->GetObjectField(TEXT("parameters")).Get();
                    SeedOwner->SetNumberField(TEXT("seed"), SeedOwner->GetNumberField(TEXT("seed")) + 1);
                });
                FRecordedFixture ChangedSeed;
                if (!Recorder.Record(Id + TEXT("-other-seed"), WithAnotherSeed, ChangedSeed, false)) return false;
                TestTrue(Id + TEXT(" changing the seed changes PCM"), !SamePcm(Primary.Capture, ChangedSeed.Capture));
            }
        }
        return true;
    }
}
#endif
