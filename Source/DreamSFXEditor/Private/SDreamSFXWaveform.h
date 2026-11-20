#pragma once

#include "Widgets/SLeafWidget.h"
#include "DreamSFXGraphIR.h"

struct FDreamSFXWaveformData
{
    TArray<float> LeftMin,LeftMax,RightMin,RightMax;
    int64 Frames=0;
    float Peak=0;
    double RMS=0;
    int32 SampleRate=48000;
    bool bFinished=false,bFinite=true,bPrepared=false;
    static FDreamSFXWaveformData Render(DreamSFX::V2::FGraphPtr Graph);
};

class SDreamSFXWaveform : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SDreamSFXWaveform){} SLATE_END_ARGS()
    void Construct(const FArguments&){}
    void SetData(TSharedPtr<const FDreamSFXWaveformData> InData){Data=MoveTemp(InData);Invalidate(EInvalidateWidgetReason::Paint);}
    FVector2D ComputeDesiredSize(float) const override{return FVector2D(480,220);}
    int32 OnPaint(const FPaintArgs&,const FGeometry&,const FSlateRect&,FSlateWindowElementList&,int32,const FWidgetStyle&,bool) const override;
private:
    TSharedPtr<const FDreamSFXWaveformData> Data;
};
