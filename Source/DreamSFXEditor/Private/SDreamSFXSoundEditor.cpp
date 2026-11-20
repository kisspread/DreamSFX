#include "SDreamSFXSoundEditor.h"
#include "DreamSFXPreviewSubsystem.h"
#include "MetasoundSource.h"
#include "Editor.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Styling/AppStyle.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "Misc/Paths.h"
#include "Misc/MessageDialog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Async/Async.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "InputCoreTypes.h"
#include "Framework/MetaData/DriverMetaData.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSlider.h"
#include "Interfaces/IPluginManager.h"

namespace DreamSFXSoundEditorPrivate
{
FString FactoryRoot(){const auto Plugin=IPluginManager::Get().FindPlugin(TEXT("DreamSFX"));return Plugin?FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir()/TEXT("Resources/Presets")):FString();}
TSharedPtr<FJsonObject> Parse(const FString& Text)
{TSharedPtr<FJsonObject> O;auto R=TJsonReaderFactory<>::Create(Text);FJsonSerializer::Deserialize(R,O);return O;}
UDreamSFXPreviewSubsystem* Preview(){return GEditor?GEditor->GetEditorSubsystem<UDreamSFXPreviewSubsystem>():nullptr;}
float PreviewGain(){const auto* P=Preview();return P?P->GetPreviewVolumeMultiplier():1.f;}
void PreviewGain(float Value){if(auto* P=Preview())P->SetPreviewVolumeMultiplier(Value);}
void SavePreviewGain(){if(auto* P=Preview())P->SavePreviewSettings();}
bool IsPlaying()
{auto* P=Preview();auto O=P?Parse(P->GetPreviewStatus()):nullptr;bool Playing=false;if(O)O->TryGetBoolField(TEXT("playing"),Playing);return Playing;}
}

void SDreamSFXSoundEditor::Construct(const FArguments& Args)
{
 LibraryRoot=Args._LibraryRoot;
 if(LibraryRoot.IsEmpty())LibraryRoot=FPaths::ConvertRelativePathToFull(IFileManager::Get().DirectoryExists(*(FPaths::ProjectDir()/TEXT("DSFX")))?FPaths::ProjectDir()/TEXT("DSFX"):DreamSFXSoundEditorPrivate::FactoryRoot());
 ChildSlot[
 SNew(SBorder).BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder"))).Padding(0)[SNew(SVerticalBox)
 +SVerticalBox::Slot().AutoHeight().Padding(12,10)[SNew(STextBlock).Text(FText::FromString(TEXT("DreamSFX Sounds"))).Font(FCoreStyle::GetDefaultFontStyle("Bold",18))]
 +SVerticalBox::Slot().AutoHeight().Padding(12,0,12,8)[SNew(SHorizontalBox)
  +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("Project sounds"))).OnClicked_Lambda([this](){const FString Root=FPaths::ProjectDir()/TEXT("DSFX");IFileManager::Get().MakeDirectory(*Root,true);OpenLibrary(Root);return FReply::Handled();})]
  +SHorizontalBox::Slot().AutoWidth().Padding(6,0)[SNew(SButton).AddMetaData(FDriverMetaData::Id(TEXT("DSFX.Factory"))).Text(FText::FromString(TEXT("Factory presets"))).OnClicked_Lambda([this](){OpenLibrary(DreamSFXSoundEditorPrivate::FactoryRoot());return FReply::Handled();})]
  +SHorizontalBox::Slot().AutoWidth().Padding(12,0)[SNew(SCheckBox).IsChecked_Lambda([this](){return GroupByCategory?ECheckBoxState::Checked:ECheckBoxState::Unchecked;}).OnCheckStateChanged_Lambda([this](ECheckBoxState State){GroupByCategory=State==ECheckBoxState::Checked;RefreshTree();})[SNew(STextBlock).Text(FText::FromString(TEXT("Group by category")))]]]
 +SVerticalBox::Slot().AutoHeight().Padding(12,0,12,8)[SNew(SHorizontalBox)
  +SHorizontalBox::Slot().FillWidth(1)[SAssignNew(RootBox,SEditableTextBox).Text(FText::FromString(LibraryRoot)).HintText(FText::FromString(TEXT("Sound source folder")))]
  +SHorizontalBox::Slot().AutoWidth().Padding(6,0)[SNew(SButton).Text(FText::FromString(TEXT("Open folder"))).OnClicked_Lambda([this](){OpenLibrary(RootBox->GetText().ToString());return FReply::Handled();})]]
 +SVerticalBox::Slot().AutoHeight().Padding(12,0,12,8)[SAssignNew(SearchBox,SSearchBox).AddMetaData(FDriverMetaData::Id(TEXT("DSFX.Search"))).HintText(FText::FromString(TEXT("Search sounds or categories"))).OnTextChanged_Lambda([this](const FText& Text){Filter=Text.ToString().TrimStartAndEnd();FilterAt=FPlatformTime::Seconds()+.15;})]
 +SVerticalBox::Slot().FillHeight(1).Padding(12,0)[SNew(SSplitter)
  +SSplitter::Slot().Value(.24f)[SAssignNew(Tree,STreeView<TSharedPtr<FDreamSFXSoundTreeNode>>).TreeItemsSource(&Roots).OnGenerateRow(this,&SDreamSFXSoundEditor::Row)
   .OnGetChildren_Lambda([](TSharedPtr<FDreamSFXSoundTreeNode> N,TArray<TSharedPtr<FDreamSFXSoundTreeNode>>& Out){Out=N->Children;}).OnSelectionChanged(this,&SDreamSFXSoundEditor::Select)
   .OnMouseButtonDoubleClick_Lambda([this](auto Node){if(Node&&!Node->Id.IsEmpty()&&(Document.Id()==Node->Id||DesiredId==Node->Id))Play();})]
  +SSplitter::Slot().Value(.76f)[SNew(SVerticalBox)
   +SVerticalBox::Slot().AutoHeight().Padding(16,0,0,4)[SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold",16)).AutoWrapText(true).Text_Lambda([this](){return FText::FromString(Document.IsLoaded()?(Document.IsInstance()?TEXT("Instance: "):TEXT("Definition: "))+Document.DisplayName():TEXT("Select a sound"));})]
   +SVerticalBox::Slot().AutoHeight().Padding(16,0,0,8)[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this](){return FText::FromString(Document.IsInstance()?TEXT("Parent: ")+Document.ParentName()+TEXT("   |   Unchecked parameters inherit the parent."):TEXT("Edit the definition in your external editor. Create an instance to tune its sound."));})]
   +SVerticalBox::Slot().AutoHeight().Padding(16,0,0,8)[SNew(SHorizontalBox)
    +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).AddMetaData(FDriverMetaData::Id(TEXT("DSFX.Play"))).Text(FText::FromString(TEXT("Play"))).IsEnabled_Lambda([this](){return Document.IsLoaded()&&!IsPreparing()&&!SelectionFailed;}).OnClicked(this,&SDreamSFXSoundEditor::Play)]
    +SHorizontalBox::Slot().AutoWidth().Padding(6,0)[SNew(SButton).AddMetaData(FDriverMetaData::Id(TEXT("DSFX.Stop"))).Text(FText::FromString(TEXT("Stop"))).OnClicked(this,&SDreamSFXSoundEditor::Stop)]
    +SHorizontalBox::Slot().AutoWidth().Padding(12,0)[SNew(SCheckBox).AddMetaData(FDriverMetaData::Id(TEXT("DSFX.Loop"))).IsChecked_Lambda([this](){return LoopPreview?ECheckBoxState::Checked:ECheckBoxState::Unchecked;}).OnCheckStateChanged_Lambda([this](ECheckBoxState S){LoopPreview=S==ECheckBoxState::Checked;})[SNew(STextBlock).Text(FText::FromString(TEXT("Loop")))]]
    +SHorizontalBox::Slot().AutoWidth().Padding(12,0)[SNew(SCheckBox).IsChecked_Lambda([this](){return AutoPreview?ECheckBoxState::Checked:ECheckBoxState::Unchecked;}).OnCheckStateChanged_Lambda([this](ECheckBoxState S){AutoPreview=S==ECheckBoxState::Checked;})[SNew(STextBlock).Text(FText::FromString(TEXT("Audition changes")))]]]
   +SVerticalBox::Slot().AutoHeight().Padding(16,0,0,8)[SNew(SHorizontalBox)
    +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(STextBlock).Text(FText::FromString(TEXT("Preview volume")))]
    +SHorizontalBox::Slot().FillWidth(1).Padding(10,0).VAlign(VAlign_Center)[SNew(SSlider).AddMetaData(FDriverMetaData::Id(TEXT("DSFX.PreviewGain.Slider"))).StepSize(.005f)
     .Value_Lambda([](){return FMath::Sqrt(DreamSFXSoundEditorPrivate::PreviewGain()/16.f);})
     .OnValueChanged_Lambda([](float Value){DreamSFXSoundEditorPrivate::PreviewGain(16*Value*Value);}).OnMouseCaptureEnd_Lambda([](){DreamSFXSoundEditorPrivate::SavePreviewGain();})
     .ToolTipText(FText::FromString(TEXT("Global listening level, 0 to 16x. The source waveform keeps its original gain."))) ]
    +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SSpinBox<float>).AddMetaData(FDriverMetaData::Id(TEXT("DSFX.PreviewGain.Value"))).MinValue(0).MaxValue(16).Delta(.1f).MaxFractionalDigits(1)
     .Value_Lambda([](){return DreamSFXSoundEditorPrivate::PreviewGain();}).OnValueChanged_Lambda([](float Value){DreamSFXSoundEditorPrivate::PreviewGain(Value);})
     .OnValueCommitted_Lambda([](float Value,ETextCommit::Type){DreamSFXSoundEditorPrivate::PreviewGain(Value);DreamSFXSoundEditorPrivate::SavePreviewGain();})
     .OnEndSliderMovement_Lambda([](float Value){DreamSFXSoundEditorPrivate::PreviewGain(Value);DreamSFXSoundEditorPrivate::SavePreviewGain();})]
    +SHorizontalBox::Slot().AutoWidth().Padding(4,0).VAlign(VAlign_Center)[SNew(STextBlock).Text(FText::FromString(TEXT("x")))]
    +SHorizontalBox::Slot().AutoWidth().Padding(8,0)[SNew(SButton).Text(FText::FromString(TEXT("1x"))).OnClicked_Lambda([](){DreamSFXSoundEditorPrivate::PreviewGain(1);DreamSFXSoundEditorPrivate::SavePreviewGain();return FReply::Handled();})]]
   +SVerticalBox::Slot().AutoHeight().Padding(16,0,0,4)[SAssignNew(Waveform,SDreamSFXWaveform)]
   +SVerticalBox::Slot().AutoHeight().Padding(16,0,0,10)[SNew(STextBlock).Text_Lambda([this](){if(PendingWave||WaveFuture.IsValid())return FText::FromString(TEXT("Updating waveform…"));if(!WaveData)return FText::FromString(TEXT("Stereo waveform"));return FText::FromString(FString::Printf(TEXT("L / R   %.3f s   Peak %.4f   RMS %.4f%s"),double(WaveData->Frames)/WaveData->SampleRate,WaveData->Peak,WaveData->RMS,WaveData->bFinished?TEXT(""):TEXT("   (first 30 seconds)")));})]
   +SVerticalBox::Slot().FillHeight(1).Padding(16,0,0,0)[SNew(SScrollBox)+SScrollBox::Slot()[SAssignNew(Controls,SVerticalBox)]]
   +SVerticalBox::Slot().AutoHeight().Padding(16,8,0,8)[SNew(SHorizontalBox)
    +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).AddMetaData(FDriverMetaData::Id(TEXT("DSFX.Save"))).Text(FText::FromString(TEXT("Save instance"))).IsEnabled_Lambda([this](){return Document.IsInstance()&&Document.IsDirty();}).OnClicked(this,&SDreamSFXSoundEditor::Save)]
    +SHorizontalBox::Slot().AutoWidth().Padding(6,0)[SNew(SButton).Text(FText::FromString(TEXT("Reload"))).OnClicked(this,&SDreamSFXSoundEditor::Reload)]
    +SHorizontalBox::Slot().AutoWidth().Padding(6,0)[SNew(SButton).Text(FText::FromString(TEXT("Build MetaSound asset")))
     .IsEnabled_Lambda([this](){return Document.IsLoaded()&&!Document.IsDirty()&&!IsPreparing()&&!SelectionFailed;})
     .OnClicked_Lambda([this](){FString Error;auto* Source=Document.SaveAsset(Error);Status=Source?TEXT("Saved ")+Source->GetPathName():Error;return FReply::Handled();})]
    +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("Open source externally"))).IsEnabled_Lambda([this](){return Document.IsLoaded()&&!IsPreparing()&&!SelectionFailed;}).OnClicked(this,&SDreamSFXSoundEditor::OpenSource)]]
   +SVerticalBox::Slot().AutoHeight().Padding(16,0,0,8)[SNew(SHorizontalBox)
    +SHorizontalBox::Slot().FillWidth(1)[SAssignNew(NewName,SEditableTextBox).AddMetaData(FDriverMetaData::Id(TEXT("DSFX.NewName"))).HintText_Lambda([this](){return FText::FromString(TEXT("New instance name  ·  ")+(IsFactoryLibrary()?TEXT("Project / Instances"):SelectedFolder));})]
    +SHorizontalBox::Slot().AutoWidth().Padding(6,0)[SNew(SButton).AddMetaData(FDriverMetaData::Id(TEXT("DSFX.Create"))).Text_Lambda([this](){return FText::FromString(IsFactoryLibrary()?TEXT("Create project instance"):TEXT("Create instance"));}).IsEnabled_Lambda([this](){return Document.IsLoaded()&&!Document.IsDirty()&&NewName.IsValid()&&!NewName->GetText().ToString().TrimStartAndEnd().IsEmpty();}).OnClicked(this,&SDreamSFXSoundEditor::CreateInstance)]]
  ]]
 +SVerticalBox::Slot().AutoHeight().Padding(12,8)[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this](){return FText::FromString((Document.IsDirty()?TEXT("Unsaved instance changes. "):TEXT(""))+Status);})]
 ]];
 RequestIndex();
}

SDreamSFXSoundEditor::~SDreamSFXSoundEditor(){if(IndexFuture.IsValid())IndexFuture.Wait();if(SelectionFuture.IsValid())SelectionFuture.Wait();if(PlaybackCheck.IsValid())PlaybackCheck.Wait();if(OwnsPreview)if(auto* P=DreamSFXSoundEditorPrivate::Preview())P->StopPreview();}
bool SDreamSFXSoundEditor::IsFactoryLibrary() const{return FPaths::IsSamePath(LibraryRoot,DreamSFXSoundEditorPrivate::FactoryRoot());}
void SDreamSFXSoundEditor::OpenLibrary(const FString& Root)
{
 if(!CanClose())return;
 Stop();Watcher.Stop();LibraryRoot=FPaths::ConvertRelativePathToFull(Root);RootBox->SetText(FText::FromString(LibraryRoot));Document=FDreamSFXSoundDocument{};Library.Reset();DesiredId.Reset();++SelectionRevision;PendingSelection=false;Roots.Reset();Tree->RequestTreeRefresh();NewName->SetText(FText::GetEmpty());Filter.Empty();SearchBox->SetText(FText::GetEmpty());Controls->ClearChildren();WaveData.Reset();Waveform->SetData(nullptr);++Revision;PendingWave=false;SelectedFolder=TEXT("Instances");RequestIndex();
}
bool SDreamSFXSoundEditor::CanClose()
{
 if(!Document.IsDirty())return true;
 const auto Answer=FMessageDialog::Open(EAppMsgType::YesNoCancel,FText::FromString(TEXT("Save changes to this sound instance?\nYes saves the instance. No discards these parameter edits.")));
 if(Answer==EAppReturnType::Cancel)return false;if(Answer==EAppReturnType::No)return true;
 FString Error;if(Document.Save(Error))return true;Status=Error;return false;
}
void SDreamSFXSoundEditor::RefreshTree()
{
 if(!Library)return;
 LibraryRoot=Library->Root;Roots.Reset();TMap<FString,TSharedPtr<FDreamSFXSoundTreeNode>> Folders;
 for(const auto& Entry:Library->Entries)
 {
  if(Entry.Kind!=TEXT("definition")&&Entry.Kind!=TEXT("instance"))continue;
  FString Relative=Entry.Path;FString Prefix=LibraryRoot;FPaths::NormalizeDirectoryName(Prefix);Prefix+=TEXT("/");FPaths::MakePathRelativeTo(Relative,*Prefix);const FString Folder=FPaths::GetPath(Relative);
  TArray<FString> Segments,Category;Library->TryCategory(Entry,Category);
  FString Searchable=Entry.Id+TEXT(" ")+Entry.Meta.DisplayName+TEXT(" ")+Entry.Meta.Description+TEXT(" ")+FString::Join(Category,TEXT(" / "));
  if(!Filter.IsEmpty()&&!Searchable.Contains(Filter))continue;
  if(GroupByCategory){Segments=Category;if(Segments.IsEmpty())Segments.Add(TEXT("Uncategorized"));}else Folder.ParseIntoArray(Segments,TEXT("/"),true);
  FString Key;TSharedPtr<FDreamSFXSoundTreeNode> Parent;
  for(const auto& Segment:Segments)
  {
   Key+=FString::Printf(TEXT("%d:%s;"),Segment.Len(),*Segment.ToLower());
   auto* Found=Folders.Find(Key);TSharedPtr<FDreamSFXSoundTreeNode> Node=Found?*Found:nullptr;
   if(!Node){Node=MakeShared<FDreamSFXSoundTreeNode>();Node->Name=Segment;Node->GroupKey=Key;Node->RelativeFolder=GroupByCategory?TEXT("Instances"):(Parent?Parent->RelativeFolder/Segment:Segment);Folders.Add(Key,Node);if(Parent)Parent->Children.Add(Node);else Roots.Add(Node);}Parent=Node;
  }
  auto Node=MakeShared<FDreamSFXSoundTreeNode>();const FString File=FPaths::GetCleanFilename(Entry.Path);Node->Name=Entry.Meta.DisplayName.IsEmpty()?File.LeftChop(File.EndsWith(TEXT(".instance.dsfx"))?14:5):Entry.Meta.DisplayName;Node->Id=Entry.Id;Node->Kind=Entry.Kind;Node->RelativeFolder=Folder;
  Node->Tooltip=Entry.Meta.Description+TEXT("\n")+FString::Join(Category,TEXT(" / "))+TEXT("\n")+Relative;
  if(Parent)Parent->Children.Add(Node);else Roots.Add(Node);
 }
 TFunction<void(TArray<TSharedPtr<FDreamSFXSoundTreeNode>>&)> Sort=[&](auto& Nodes)
 {Nodes.Sort([](const auto& A,const auto& B){if(A->Id.IsEmpty()!=B->Id.IsEmpty())return A->Id.IsEmpty();return A->Name.Compare(B->Name,ESearchCase::IgnoreCase)<0;});for(auto& N:Nodes){Sort(N->Children);N->SoundCount=N->Id.IsEmpty()?0:1;for(const auto& Child:N->Children)N->SoundCount+=Child->SoundCount;}};Sort(Roots);
 Tree->RequestTreeRefresh();for(const auto& P:Folders)Tree->SetItemExpansion(P.Value,!Filter.IsEmpty()||Roots.Num()==1);
 TFunction<bool(TSharedPtr<FDreamSFXSoundTreeNode>)> Reveal=[&](auto Node){if(Node->Id==Document.Id()&&!Node->Id.IsEmpty()){Tree->RequestScrollIntoView(Node);return true;}for(const auto& Child:Node->Children)if(Reveal(Child)){Tree->SetItemExpansion(Node,true);return true;}return false;};
 if(Document.IsLoaded())for(const auto& Node:Roots)if(Reveal(Node))break;
 Status=GroupByCategory?TEXT("Categories follow the definition folders. Select a sound to audition."):TEXT("Showing source folders. Select a sound to audition.");

}
TSharedRef<ITableRow> SDreamSFXSoundEditor::Row(TSharedPtr<FDreamSFXSoundTreeNode> N,const TSharedRef<STableViewBase>& Owner)
{return SNew(STableRow<TSharedPtr<FDreamSFXSoundTreeNode>>,Owner).AddMetaData(FDriverMetaData::Id(FName(*(N->Id.IsEmpty()?TEXT("DSFX.Group.")+N->GroupKey:TEXT("DSFX.Entry.")+N->Id)))).ToolTipText(FText::FromString(N->Tooltip))[SNew(STextBlock).Text(FText::FromString(N->Id.IsEmpty()?N->Name+FString::Printf(TEXT(" (%d)"),N->SoundCount):N->Name+(N->Kind==TEXT("instance")?TEXT("  · instance"):TEXT("  · definition"))))];}
void SDreamSFXSoundEditor::Select(TSharedPtr<FDreamSFXSoundTreeNode> N,ESelectInfo::Type)
{
 if(!N)return;if(N->Id.IsEmpty()){SelectedFolder=N->RelativeFolder;return;}
 if(N->Id==DesiredId&&(IsPreparing()||Document.Id()==N->Id))return;
 if(!CanClose())return;
 Stop();DesiredId=N->Id;DesiredFolder=N->RelativeFolder.IsEmpty()?TEXT("Instances"):N->RelativeFolder;
 ++SelectionRevision;PendingSelection=true;SelectionFailed=true;Status=TEXT("Preparing sound…");
}
void SDreamSFXSoundEditor::RebuildControls()
{
 Controls->ClearChildren();FString Group;
 for(const auto& P:Document.Parameters())
 {
  const FString NewGroup=P.Node.ToString();
  if(NewGroup!=Group){Controls->AddSlot().AutoHeight().Padding(0,10,0,4)[SNew(STextBlock).Text(FText::FromString(NewGroup)).Font(FCoreStyle::GetDefaultFontStyle("Bold",11))];Group=NewGroup;}
  TSharedPtr<SWidget> ValueEditor;
  const FString ControlId=TEXT("DSFX.Param.")+P.Key;
  if(P.bInteger&&P.Maximum<=MAX_int32&&P.Minimum>=MIN_int32)
   ValueEditor=SNew(SSpinBox<int32>).MinValue(int32(P.Minimum)).MaxValue(int32(P.Maximum)).MinSliderValue(int32(P.Minimum)).MaxSliderValue(int32(P.Maximum)).Delta(1)
    .IsEnabled_Lambda([this,P](){return !IsPreparing()&&Document.IsInstance()&&Document.HasOverride(P);}).Value_Lambda([this,P](){return int32(Document.Value(P));}).OnValueCommitted_Lambda([this,P](int32 V,ETextCommit::Type){Change(P,double(V));}).OnEndSliderMovement_Lambda([this,P](int32 V){Change(P,double(V));});
  else
   ValueEditor=SNew(SSpinBox<double>).MinValue(P.Minimum).MaxValue(P.Maximum).MinSliderValue(P.Minimum).MaxSliderValue(P.Maximum).Delta(.01).MinFractionalDigits(0).MaxFractionalDigits(3)
    .IsEnabled_Lambda([this,P](){return !IsPreparing()&&Document.IsInstance()&&Document.HasOverride(P);}).Value_Lambda([this,P](){return Document.Value(P);}).OnValueCommitted_Lambda([this,P](double V,ETextCommit::Type){Change(P,V);}).OnEndSliderMovement_Lambda([this,P](double V){Change(P,V);});
  ValueEditor->AddMetadata(FDriverMetaData::Id(FName(*(ControlId+TEXT(".Value")))));
  Controls->AddSlot().AutoHeight().Padding(0,3)[SNew(SHorizontalBox)
   +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SCheckBox).AddMetaData(FDriverMetaData::Id(FName(*(ControlId+TEXT(".Override"))))).IsEnabled_Lambda([this](){return !IsPreparing()&&Document.IsInstance();}).IsChecked_Lambda([this,P](){return Document.HasOverride(P)?ECheckBoxState::Checked:ECheckBoxState::Unchecked;}).OnCheckStateChanged_Lambda([this,P](ECheckBoxState S){Change(P,S==ECheckBoxState::Checked?TOptional<double>(Document.Value(P)):TOptional<double>());})]
   +SHorizontalBox::Slot().FillWidth(1).Padding(6,0).VAlign(VAlign_Center)[SNew(STextBlock).Text(FText::FromString(P.Label))]
   +SHorizontalBox::Slot().FillWidth(.65f)[ValueEditor.ToSharedRef()]
   +SHorizontalBox::Slot().AutoWidth().Padding(6,0).VAlign(VAlign_Center)[SNew(STextBlock).Text(FText::FromString(P.Unit))]
  ];
 }
}
void SDreamSFXSoundEditor::Change(const FDreamSFXInstanceParameter& P,TOptional<double> Value)
{
 if(IsPreparing())return;
 ++SelectionRevision;FString Error;if(!Document.SetOverride(P,Value,Error)){Status=Error;return;}QueueWaveform();
 if(AutoPreview&&OwnsPreview)AuditionAt=FPlatformTime::Seconds()+.2;
 else if(OwnsPreview)Stop();
 Status=TEXT("Parameter changed. Save the instance to keep these overrides.");
}
void SDreamSFXSoundEditor::QueueWaveform(){++Revision;PendingWave=true;Waveform->SetRenderOpacity(.45f);}
void SDreamSFXSoundEditor::Tick(const FGeometry& G,double Now,float Delta)
{
 SCompoundWidget::Tick(G,Now,Delta);
 if(Watcher.ConsumeChange())
 {const bool Resume=OwnsPreview&&AutoPreview;Stop();PlayWhenReady=Resume;NextRefresh=FPlatformTime::Seconds()+.35;++SelectionRevision;}
 if(NextRefresh>0&&FPlatformTime::Seconds()>=NextRefresh){NextRefresh=0;RequestIndex();}
 PumpPreparation();
 if(FilterAt>0&&FPlatformTime::Seconds()>=FilterAt){FilterAt=0;RefreshTree();}
 if(WaveFuture.IsValid()&&WaveFuture.IsReady())
 {
  auto Data=WaveFuture.Get();WaveFuture=TFuture<FDreamSFXWaveformData>();
  if(RenderingRevision==Revision){WaveData=MakeShared<FDreamSFXWaveformData>(MoveTemp(Data));Waveform->SetData(WaveData);Waveform->SetRenderOpacity(1);if(!WaveData->bPrepared||!WaveData->bFinite)Status=TEXT("Waveform rendering failed; check the source definition.");}
 }
 if(PendingWave&&!WaveFuture.IsValid()&&Document.IsLoaded())
 {const auto IR=Document.GetGraph();RenderingRevision=Revision;PendingWave=false;WaveFuture=Async(EAsyncExecution::ThreadPool,[IR](){return FDreamSFXWaveformData::Render(IR);});}
 if(AuditionAt>0&&FPlatformTime::Seconds()>=AuditionAt){AuditionAt=0;Play();}

}
void SDreamSFXSoundEditor::RequestIndex()
{
 ++IndexRevision;++SelectionRevision;PendingIndex=true;PendingSelection=!DesiredId.IsEmpty()||Document.IsLoaded();SelectionFailed=PendingSelection;
 if(DesiredId.IsEmpty())DesiredId=Document.Id();
 if(!Watcher.IsWatching())Watcher.Start(LibraryRoot);
 Status=TEXT("Refreshing sound library…");
}
void SDreamSFXSoundEditor::PumpPreparation()
{
 if(IndexFuture.IsValid()&&IndexFuture.IsReady())
 {
  auto Ready=IndexFuture.Get();IndexFuture={};
  if(IndexWorkRevision==IndexRevision)
  {
   if(!Ready->Error.IsEmpty()){Status=Ready->Error;PendingSelection=false;PlayWhenReady=false;SelectionFailed=true;}
   else {Library=MoveTemp(Ready);RefreshTree();}
  }
 }
 if(PendingIndex&&!IndexFuture.IsValid())
 {
  PendingIndex=false;IndexWorkRevision=IndexRevision;const FString Root=LibraryRoot;
  IndexFuture=Async(EAsyncExecution::ThreadPool,[Root]()->FDreamSFXSoundDocument::FSnapshot{auto Next=MakeShared<FDreamSFXLibrarySnapshot,ESPMode::ThreadSafe>();Next->Load(Root);return Next;});
 }
 if(SelectionFuture.IsValid()&&SelectionFuture.IsReady())
 {
  auto Ready=SelectionFuture.Get();SelectionFuture={};
  if(SelectionWorkRevision==SelectionRevision)
  {
   if(!Ready.Error.IsEmpty()){Status=Ready.Error;PlayWhenReady=false;SelectionFailed=true;}
   else
   {
    Document=MoveTemp(Ready.Document);SelectionFailed=false;SelectedFolder=DesiredFolder;RebuildControls();QueueWaveform();
    Status=Document.IsInstance()?TEXT("Enable parameters to override the parent."):TEXT("Double-click a sound or press Play to audition.");
   }
  }
 }
 if(PendingSelection&&Library&&!PendingIndex&&!IndexFuture.IsValid()&&!SelectionFuture.IsValid())
 {
  PendingSelection=false;SelectionWorkRevision=SelectionRevision;
  const auto Snapshot=Library;const FString Id=DesiredId,Draft=Document.Id()==Id&&Document.IsDirty()?Document.DraftText():FString(),Baseline=Document.BaselineText();
  SelectionFuture=Async(EAsyncExecution::ThreadPool,[Snapshot,Id,Draft,Baseline](){FPreparedDocument Ready;Ready.Document.LoadFromSnapshot(Snapshot,Id,Ready.Error,Draft,Baseline);return Ready;});
 }
 if(PlaybackCheck.IsValid()&&PlaybackCheck.IsReady())
 {
  const FString Error=PlaybackCheck.Get();PlaybackCheck={};
  if(PlaybackRevision==SelectionRevision&&!IsPreparing()&&PlayWhenReady)
  {
   if(Error.IsEmpty()&&NextRefresh==0&&!SelectionFailed){PlayWhenReady=false;StartPreparedPlayback();}
   else {RequestIndex();Status=Error;}
  }
 }
 if(PlayWhenReady&&!SelectionFailed&&!IsPreparing()&&!PlaybackCheck.IsValid()&&NextRefresh==0&&Document.IsLoaded())
 {
  PlaybackRevision=SelectionRevision;const auto Snapshot=Document.Snapshot();const auto Dependencies=Document.Dependencies();const auto Id=Document.Id();
  PlaybackCheck=Async(EAsyncExecution::ThreadPool,[Snapshot,Dependencies,Id](){FString Error;Snapshot->AreSourcesCurrent(Id,Dependencies,Error);return Error;});
 }
}
FReply SDreamSFXSoundEditor::Play()
{
 if(SelectionFailed&&!IsPreparing())return FReply::Handled();
 PlayWhenReady=true;Status=TEXT("Preparing preview…");return FReply::Handled();
}
void SDreamSFXSoundEditor::StartPreparedPlayback()
{
 FString Error;auto* Source=Document.PreviewSource(Error);if(!Source){Status=Error;return;}
 if(auto* P=DreamSFXSoundEditorPrivate::Preview())
 {auto O=DreamSFXSoundEditorPrivate::Parse(P->PreviewPreparedSource(Source,LoopPreview?0:1));bool Ok=false;if(O&&O->TryGetBoolField(TEXT("ok"),Ok)&&Ok){OwnsPreview=true;Status=TEXT("Preview playing.");}else{FString Message;if(O)O->TryGetStringField(TEXT("message"),Message);Status=Message.IsEmpty()?TEXT("Preview could not start."):Message;}}
}

FReply SDreamSFXSoundEditor::Stop(){PlayWhenReady=false;AuditionAt=0;if(auto* P=DreamSFXSoundEditorPrivate::Preview())P->StopPreview();OwnsPreview=false;Status=TEXT("Preview stopped.");return FReply::Handled();}
FReply SDreamSFXSoundEditor::Save(){if(IsPreparing())return FReply::Handled();FString Error;if(Document.Save(Error)){RequestIndex();Status=TEXT("Instance text saved.");}else Status=Error;return FReply::Handled();}
FReply SDreamSFXSoundEditor::Reload(){if(!CanClose())return FReply::Handled();Stop();Document=FDreamSFXSoundDocument{};RequestIndex();return FReply::Handled();}
FReply SDreamSFXSoundEditor::CreateInstance()
{
 if(IsPreparing())return FReply::Handled();
 FString Error;const FString Name=NewName->GetText().ToString();const bool Created=IsFactoryLibrary()?Document.CreateProjectInstance(FPaths::ProjectDir()/TEXT("DSFX"),Name,Error):Document.CreateInstance(SelectedFolder,Name,Error);
 if(Created){Stop();Watcher.Stop();LibraryRoot=Document.Root();RootBox->SetText(FText::FromString(LibraryRoot));Filter.Empty();SearchBox->SetText(FText::GetEmpty());DesiredId=Document.Id();RequestIndex();RebuildControls();QueueWaveform();Status=TEXT("Instance created. Enable parameters to start tuning.");NewName->SetText(FText::GetEmpty());}else Status=Error;
 return FReply::Handled();
}
FReply SDreamSFXSoundEditor::OpenSource(){if(Document.IsLoaded())FPlatformProcess::LaunchFileInDefaultExternalApplication(*Document.Path());return FReply::Handled();}
FReply SDreamSFXSoundEditor::OnKeyDown(const FGeometry&,const FKeyEvent& Event)
{if(Event.IsControlDown()&&Event.GetKey()==EKeys::S)return Save();return FReply::Unhandled();}
