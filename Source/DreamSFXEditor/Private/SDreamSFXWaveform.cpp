#include "SDreamSFXWaveform.h"
#include "DreamSFXGraphInstance.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"

FDreamSFXWaveformData FDreamSFXWaveformData::Render(DreamSFX::V2::FGraphPtr Graph)
{
    FDreamSFXWaveformData Data;
    DreamSFX::V2::FGraphInstance Instance;
    FString Error;
    Data.bPrepared=Instance.Prepare(Graph,Data.SampleRate,Error);
    if(!Data.bPrepared)return Data;
    const int64 Limit=FMath::Max<int64>(1,Instance.GetLengthFrames());
    const int32 Buckets=int32(FMath::Min<int64>(2048,Limit));
    Data.LeftMin.SetNumZeroed(Buckets);Data.LeftMax.SetNumZeroed(Buckets);Data.RightMin.SetNumZeroed(Buckets);Data.RightMax.SetNumZeroed(Buckets);
    double Energy=0;
    while(!Instance.IsFinished()&&Data.Frames<Limit)
    {
        const int32 Count=int32(FMath::Min<int64>(256,Limit-Data.Frames));
        float Samples[512];
        const int32 Used=Instance.Render(Samples,Count);
        for(int32 F=0;F<Used;++F)
        {
            const int32 Bucket=int32(FMath::Min<int64>(Buckets-1,(Data.Frames+F)*Buckets/Limit));
            const float L=Samples[F*2],R=Samples[F*2+1];
            Data.bFinite&=FMath::IsFinite(L)&&FMath::IsFinite(R);Data.Peak=FMath::Max(Data.Peak,FMath::Max(FMath::Abs(L),FMath::Abs(R)));
            Data.LeftMin[Bucket]=FMath::Min(Data.LeftMin[Bucket],L);Data.LeftMax[Bucket]=FMath::Max(Data.LeftMax[Bucket],L);
            Data.RightMin[Bucket]=FMath::Min(Data.RightMin[Bucket],R);Data.RightMax[Bucket]=FMath::Max(Data.RightMax[Bucket],R);
            Energy+=double(L)*L+double(R)*R;
        }
        Data.Frames+=Used;if(Used<Count)break;
    }
    Data.bFinished=Instance.IsFinished();Data.RMS=Data.Frames>0?FMath::Sqrt(Energy/(Data.Frames*2)):0;
    const int32 UsedBuckets=int32(FMath::Clamp<int64>((Data.Frames*Buckets+Limit-1)/Limit,1,Buckets));
    Data.LeftMin.SetNum(UsedBuckets);Data.LeftMax.SetNum(UsedBuckets);Data.RightMin.SetNum(UsedBuckets);Data.RightMax.SetNum(UsedBuckets);
    return Data;
}

int32 SDreamSFXWaveform::OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle&,bool) const
{
    const FVector2f Size=FVector2f(G.GetLocalSize());
    FSlateDrawElement::MakeBox(Out,Layer,G.ToPaintGeometry(),FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")),ESlateDrawEffect::None,FLinearColor(.025f,.035f,.045f));
    for(int32 Channel=0;Channel<2;++Channel)
    {
        const float Center=Size.Y*(Channel==0?.25f:.75f),HalfHeight=Size.Y*.22f;
        TArray<FVector2D> Axis{{0,Center},{Size.X,Center}};
        FSlateDrawElement::MakeLines(Out,Layer+1,G.ToPaintGeometry(),Axis,ESlateDrawEffect::None,FLinearColor(.15f,.2f,.25f),true,1);
        if(!Data)continue;
        const auto& Min=Channel==0?Data->LeftMin:Data->RightMin;const auto& Max=Channel==0?Data->LeftMax:Data->RightMax;
        const int32 Pixels=FMath::Max(1,FMath::FloorToInt(Size.X));
        TArray<FVector2D> Envelope;Envelope.Reserve(Pixels*2);
        for(int32 X=0;X<Pixels&&!Min.IsEmpty();++X)
        {
            const int32 Begin=X*Min.Num()/Pixels,End=FMath::Max(Begin+1,(X+1)*Min.Num()/Pixels);
            float Lo=0,Hi=0;for(int32 B=Begin;B<FMath::Min(End,Min.Num());++B){Lo=FMath::Min(Lo,Min[B]);Hi=FMath::Max(Hi,Max[B]);}
            Envelope.Add({double(X),Center-FMath::Clamp(Hi,-1.f,1.f)*HalfHeight});Envelope.Add({double(X),Center-FMath::Clamp(Lo,-1.f,1.f)*HalfHeight});
        }
        if(!Envelope.IsEmpty())FSlateDrawElement::MakeLines(Out,Layer+2,G.ToPaintGeometry(),Envelope,ESlateDrawEffect::None,FLinearColor(.25f,.78f,.68f),true,1);
    }
    return Layer+2;
}
