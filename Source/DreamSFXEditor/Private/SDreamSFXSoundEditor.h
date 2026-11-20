#pragma once

#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"
#include "DreamSFXSoundDocument.h"
#include "DreamSFXLibraryWatcher.h"
#include "SDreamSFXWaveform.h"
#include "Async/Future.h"

class SEditableTextBox;
class SVerticalBox;
class SSearchBox;
struct FDreamSFXSoundTreeNode
{
    FString Name,Id,Kind,RelativeFolder,GroupKey,Tooltip;
    int32 SoundCount=0;
    TArray<TSharedPtr<FDreamSFXSoundTreeNode>> Children;
};

class SDreamSFXSoundEditor : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SDreamSFXSoundEditor){} SLATE_ARGUMENT(FString,LibraryRoot) SLATE_END_ARGS()
    void Construct(const FArguments&);
    ~SDreamSFXSoundEditor();
    void Tick(const FGeometry&,double,float) override;
    bool CanClose();
    bool SupportsKeyboardFocus() const override{return true;}
    FReply OnKeyDown(const FGeometry&,const FKeyEvent&) override;

private:
    friend class FDreamSFXV2AuditionTransport;
    struct FPreparedDocument {FDreamSFXSoundDocument Document;FString Error;};
    FDreamSFXSoundDocument Document;
    FDreamSFXSoundDocument::FSnapshot Library;
    TFuture<FDreamSFXSoundDocument::FSnapshot> IndexFuture;
    TFuture<FPreparedDocument> SelectionFuture;
    TFuture<FString> PlaybackCheck;
    FString DesiredId,DesiredFolder;
    uint64 IndexRevision=0,IndexWorkRevision=0,SelectionRevision=0,SelectionWorkRevision=0,PlaybackRevision=0;
    bool PendingIndex=false,PendingSelection=false,PlayWhenReady=false,SelectionFailed=false;
    bool IsPreparing() const{return PendingIndex||IndexFuture.IsValid()||PendingSelection||SelectionFuture.IsValid();}
    void RequestIndex();
    void PumpPreparation();
    void StartPreparedPlayback();
    FDreamSFXLibraryWatcher Watcher;
    FString LibraryRoot,Status,Filter,SelectedFolder=TEXT("Instances");
    TArray<TSharedPtr<FDreamSFXSoundTreeNode>> Roots;
    TSharedPtr<STreeView<TSharedPtr<FDreamSFXSoundTreeNode>>> Tree;
    TSharedPtr<SEditableTextBox> RootBox,NewName;
    TSharedPtr<SSearchBox> SearchBox;
    TSharedPtr<SVerticalBox> Controls;
    TSharedPtr<SDreamSFXWaveform> Waveform;
    TSharedPtr<const FDreamSFXWaveformData> WaveData;
    TFuture<FDreamSFXWaveformData> WaveFuture;
    uint64 Revision=0,RenderingRevision=0;
    bool PendingWave=false,AutoPreview=true,LoopPreview=false,OwnsPreview=false,GroupByCategory=true;
    double NextRefresh=0,AuditionAt=0,FilterAt=0;
    void OpenLibrary(const FString& Root);
    bool IsFactoryLibrary() const;
    void RefreshTree();
    void Select(TSharedPtr<FDreamSFXSoundTreeNode>,ESelectInfo::Type);
    TSharedRef<ITableRow> Row(TSharedPtr<FDreamSFXSoundTreeNode>,const TSharedRef<STableViewBase>&);
    void RebuildControls();
    void Change(const FDreamSFXInstanceParameter&,TOptional<double>);
    void QueueWaveform();
    bool IsWaveformCurrent() const{return WaveData.IsValid()&&!PendingWave&&!WaveFuture.IsValid()&&RenderingRevision==Revision;}
    FReply Play();FReply Stop();FReply Save();FReply Reload();FReply CreateInstance();FReply OpenSource();
};
