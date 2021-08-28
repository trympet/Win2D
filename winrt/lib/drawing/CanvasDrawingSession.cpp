// Copyright (c) Microsoft Corporation. All rights reserved.
//
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.

#include "pch.h"

#include "CanvasActiveLayer.h"
#include "CanvasSpriteBatch.h"
#include "text/CanvasTextFormat.h"
#include "text/CanvasTextRenderingParameters.h"
#include "text/CanvasFontFace.h"
#include "utils/TemporaryTransform.h"
#include "text/TextUtilities.h"
#include "text/InternalDWriteTextRenderer.h"
#include "text/DrawGlyphRunHelper.h"
#include "svg/CanvasSvgDocument.h"

namespace ABI { namespace Microsoft { namespace Graphics { namespace Canvas
{
    using namespace ABI::Windows::Foundation;
    using namespace ABI::Windows::UI;

    ComPtr<ID2D1StrokeStyle1> ToD2DStrokeStyle(ICanvasStrokeStyle* strokeStyle, ID2D1DeviceContext* deviceContext)
    {
        if (!strokeStyle) return nullptr;

        ComPtr<ID2D1Factory> d2dFactory;
        deviceContext->GetFactory(&d2dFactory);

        ComPtr<ICanvasStrokeStyleInternal> internal;
        ThrowIfFailed(strokeStyle->QueryInterface(internal.GetAddressOf()));

        return internal->GetRealizedD2DStrokeStyle(d2dFactory.Get());
    }


    static D2D1_SIZE_F GetBitmapSize(D2D1_UNIT_MODE unitMode, ID2D1Bitmap* bitmap)
    {
        switch (unitMode)
        {
        case D2D1_UNIT_MODE_DIPS:
            return bitmap->GetSize();
            
        case D2D1_UNIT_MODE_PIXELS:
        {
            auto pixelSize = bitmap->GetPixelSize();
            return D2D1_SIZE_F{ static_cast<float>(pixelSize.width), static_cast<float>(pixelSize.height) };
        }

        default:
            assert(false);
            return D2D1_SIZE_F{};
        }
    }
    

    //
    // This drawing session adapter is used when wrapping an existing
    // ID2D1DeviceContext.  In this wrapper, interop, case we don't want
    // CanvasDrawingSession to call any additional methods in the device context.
    //
    class NoopCanvasDrawingSessionAdapter : public ICanvasDrawingSessionAdapter,
                                            private LifespanTracker<NoopCanvasDrawingSessionAdapter>
    {
    public:
        virtual void EndDraw(ID2D1DeviceContext1*) override
        {
            // nothing
        }
    };


    ComPtr<CanvasDrawingSession> CanvasDrawingSession::CreateNew(
        ID2D1DeviceContext1* deviceContext,
        std::shared_ptr<ICanvasDrawingSessionAdapter> drawingSessionAdapter,
        ICanvasDevice* owner,
        std::shared_ptr<bool> targetHasActiveDrawingSession,
        D2D1_POINT_2F offset)
    {
        InitializeDefaultState(deviceContext);

        auto drawingSession = Make<CanvasDrawingSession>(
            deviceContext,
            drawingSessionAdapter,
            owner,
            std::move(targetHasActiveDrawingSession),
            offset);
        CheckMakeResult(drawingSession);

        return drawingSession;
    }


    void CanvasDrawingSession::InitializeDefaultState(ID2D1DeviceContext1* deviceContext)
    {
        // Win2D wants a different text antialiasing default vs. native D2D.
        deviceContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }


#if WINVER > _WIN32_WINNT_WINBLUE
    ComPtr<IInkD2DRenderer> DefaultInkAdapter::CreateInkRenderer()
    {
        ComPtr<IInkD2DRenderer> inkRenderer;

        ThrowIfFailed(CoCreateInstance(__uuidof(InkD2DRenderer),
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&inkRenderer)));

        return inkRenderer;
    }


    bool DefaultInkAdapter::IsHighContrastEnabled()
    {
        if (!m_accessibilitySettings)
        {
            ComPtr<IActivationFactory> activationFactory;
            ThrowIfFailed(GetActivationFactory(HStringReference(RuntimeClass_Windows_UI_ViewManagement_AccessibilitySettings).Get(), &activationFactory));
            ThrowIfFailed(activationFactory->ActivateInstance(&m_accessibilitySettings));
        }

        boolean isHighContrastEnabled;
        ThrowIfFailed(m_accessibilitySettings->get_HighContrast(&isHighContrastEnabled));
        return !!isHighContrastEnabled;
    }
#endif


    CanvasDrawingSession::CanvasDrawingSession(
        ID2D1DeviceContext1* deviceContext,
        std::shared_ptr<ICanvasDrawingSessionAdapter> adapter,
        ICanvasDevice* owner,
        std::shared_ptr<bool> targetHasActiveDrawingSession,
        D2D1_POINT_2F offset)
        : ResourceWrapper(deviceContext)
        , m_adapter(adapter ? adapter : std::make_shared<NoopCanvasDrawingSessionAdapter>())
        , m_targetHasActiveDrawingSession(std::move(targetHasActiveDrawingSession))
        , m_offset(offset)
        , m_nextLayerId(0)
        , m_owner(owner)
    {
        if (m_targetHasActiveDrawingSession)
            *m_targetHasActiveDrawingSession = true;
    }


    CanvasDrawingSession::~CanvasDrawingSession()
    {
        // Ignore any errors when closing during destruction
        (void)Close();
    }


    IFACEMETHODIMP CanvasDrawingSession::Close()
    {
        return ExceptionBoundary(
            [&]
            {
                auto deviceContext = MaybeGetResource();
        
                ReleaseResource();

                if (!m_activeLayerIds.empty())
                    ThrowHR(E_FAIL, Strings::DidNotPopLayer);

                if (m_targetHasActiveDrawingSession)
                    *m_targetHasActiveDrawingSession = false;

                if (m_adapter)
                {
                    assert(deviceContext);
                    
                    // Arrange it so that m_adapter will always get
                    // reset, even if EndDraw throws.
                    auto adapter = m_adapter;
                    m_adapter.reset();

                    adapter->EndDraw(deviceContext.Get());
                }

                m_solidColorBrush.Reset();
                m_defaultTextFormat.Reset();
                m_owner.Reset();
#if WINVER > _WIN32_WINNT_WINBLUE
                m_inkD2DRenderer.Reset();
                m_inkStateBlock.Reset();
#endif
            });
    }


    IFACEMETHODIMP CanvasDrawingSession::Clear(
        Color color)
    {
        return ExceptionBoundary(
            [&]
            {
                GetResource()->Clear(ToD2DColor(color));
            });
    }


    IFACEMETHODIMP CanvasDrawingSession::ClearHdr(
        Vector4 color)
    {
        return ExceptionBoundary(
            [&]
            {
                GetResource()->Clear(ToD2DColor(color));
            });
    }


    IFACEMETHODIMP CanvasDrawingSession::Flush()
    {
        return ExceptionBoundary(
            [&]
            {
                ThrowIfFailed(GetResource()->Flush(nullptr, nullptr));
            });
    }


    // 
    // DrawImage
    //  

    IFACEMETHODIMP CanvasDrawingSession::DrawImageAtOrigin(
        ICanvasImage* image)
    {
        auto offset = Vector2{ 0, 0 };
        return DrawImageImpl(image, &offset, nullptr, nullptr, DefaultDrawImageOpacity(), DefaultDrawImageInterpolation(), nullptr);
    }


    IFACEMETHODIMP CanvasDrawingSession::DrawImageAtOffset(
        ICanvasImage* image,
        Vector2 offset)
    {
        return DrawImageImpl(image, &offset, nullptr, nullptr, DefaultDrawImageOpacity(), DefaultDrawImageInterpolation(), nullptr);
    }


    IFACEMETHODIMP CanvasDrawingSession::DrawImageAtCoords(
        ICanvasImage* image,
        float x,
        float y)
    {
        auto offset = Vector2{ x, y };
        return DrawImageImpl(image, &offset, nullptr, nullptr, DefaultDrawImageOpacity(), DefaultDrawImageInterpolation(), nullptr);
    }


    IFACEMETHODIMP CanvasDrawingSession::DrawImageToRect(
        ICanvasBitmap* bitmap,
        Rect destinationRect)
    {   
        return DrawBitmapImpl(bitmap, nullptr, &destinationRect, nullptr, DefaultDrawImageOpacity(), DefaultDrawImageInterpolation(), nullptr);
    }


    IFACEMETHODIMP CanvasDrawingSession::DrawImageAtOffsetWithSourceRect(
        ICanvasImage* image,
        Vector2 offset,
        Rect sourceRect)
    {
        return DrawImageImpl(image, &offset, nullptr, &sourceRect, DefaultDrawImageOpacity(), DefaultDrawImageInterpolation(), nullptr);
    }


    IFACEMETHODIMP CanvasDrawingSession::DrawImageAtCoordsWithSourceRect(
        ICanvasImage* image,
        float x,
        float y,
        Rect sourceRect)
    {
        auto offset = Vector2{ x, y };
        return DrawImageImpl(image, &offset, nullptr, &sourceRect, DefaultDrawImageOpacity(), DefaultDrawImageInterpolation(), nullptr);
    }


    IFACEMETHODIMP CanvasDrawingSession::DrawImageToRectWithSourceRect(
        ICanvasImage* image,
        Rect destinationRect,
        Rect sourceRect)
    {
        return DrawImageImpl(image, nullptr, &destinationRect, &sourceRect, DefaultDrawImageOpacity(), DefaultDrawImageInterpolation(), nullptr);
    }


    IFACEMETHODIMP CanvasDrawingSession::DrawImageAtOffsetWithSourceRectAndOpacity(
        ICanvasImage* image, 
        Vector2 offset,
        Rect sourceRectangle,
        float opacity)
    {
        return DrawImageImpl(image, &offset, nullptr, &sourceRectangle, opacity, DefaultDrawImageInterpolation(), nullptr);
    }

    IFACEMETHODIMP CanvasDrawingSession::DrawImageAtCoordsWithSourceRectAndOpacity(
        ICanvasImage* image, 
        float x,
        float y,
        Rect sourceRectangle,
        float opacity)
    {
        auto offset = Vector2{ x, y };
        return DrawImageImpl(image, &offset, nullptr, &sourceRectangle, opacity, DefaultDrawImageInterpolation(), nullptr);
    }

    IFACEMETHODIMP CanvasDrawingSession::DrawImageToRectWithSourceRectAndOpacity(
        ICanvasImage* image,
        Rect destinationRectangle,
        Rect sourceRectangle,
        float opacity)
    {
        return DrawImageImpl(image, nullptr, &destinationRectangle, &sourceRectangle, opacity, DefaultDrawImageInterpolation(), nullptr);
    }

    IFACEMETHODIMP CanvasDrawingSession::DrawImageAtOffsetWithSourceRectAndOpacityAndInterpolation(
        ICanvasImage* image, 
        Vector2 offset,
        Rect sourceRectangle,
        float opacity,
        CanvasImageInterpolation interpolation)
    {
        return DrawImageImpl(image, &offset, nullptr, &sourceRectangle, opacity, interpolation, nullptr);
    }

    IFACEMETHODIMP CanvasDrawingSession::DrawImageAtCoordsWithSourceRectAndOpacityAndInterpolation(
        ICanvasImage* image, 
        float x,
        float y,
        Rect sourceRectangle,
        float opacity,
        CanvasImageInterpolation interpolation)
    {
        auto offset = Vector2{ x, y };
        return DrawImageImpl(image, &offset, nullptr, &sourceRectangle, opacity, interpolation, nullptr);
    }
    
    IFACEMETHODIMP CanvasDrawingSession::DrawImageToRectWithSourceRectAndOpacityAndInterpolation(
        ICanvasImage* image,
        Rect destinationRectangle,
        Rect sourceRectangle,
        float opacity,
        CanvasImageInterpolation interpolation)
    {
        return DrawImageImpl(image, nullptr, &destinationRectangle, &sourceRectangle, opacity, interpolation, nullptr);
    }

    IFACEMETHODIMP CanvasDrawingSession::DrawImageAtOffsetWithSourceRectAndOpacityAndInterpolationAndComposite(
        ICanvasImage* image, 
        Vector2 offset,
        Rect sourceRectangle,
        float opacity,
        CanvasImageInterpolation interpolation,
        CanvasComposite composite)
    {
        return DrawImageImpl(image, &offset, nullptr, &sourceRectangle, opacity, interpolation, &composite);
    }

    IFACEMETHODIMP CanvasDrawingSession::DrawImageAtCoordsWithSourceRectAndOpacityAndInterpolationAndComposite(
        ICanvasImage* image, 
        float x,
        float y,
        Rect sourceRectangle,
        float opacity,
        CanvasImageInterpolation interpolation,
        CanvasComposite composite)
    {
        auto offset = Vector2{ x, y };
        return DrawImageImpl(image, &offset, nullptr, &sourceRectangle, opacity, interpolation, &composite);
    }

    IFACEMETHODIMP CanvasDrawingSession::DrawImageToRectWithSourceRectAndOpacityAndInterpolationAndComposite(
        ICanvasImage* image,
        Rect destinationRectangle,
        Rect sourceRectangle,
        float opacity,
        CanvasImageInterpolation interpolation,
        CanvasComposite composite)
    {
        return DrawImageImpl(image, nullptr, &destinationRectangle, &sourceRectangle, opacity, interpolation, &composite);
    }

    IFACEMETHODIMP CanvasDrawingSession::DrawImageAtOffsetWithSourceRectAndOpacityAndInterpolationAndPerspective(
        ICanvasBitmap* bitmap, 
        Vector2 offset,
        Rect sourceRectangle,
        float opacity,
        CanvasImageInterpolation interpolation,
        Matrix4x4 perspective)
    {
        return DrawBitmapImpl(bitmap, &offset, nullptr, &sourceRectangle, opacity, interpolation, &perspective);
    }

    IFACEMETHODIMP CanvasDrawingSession::DrawImageAtCoordsWithSourceRectAndOpacityAndInterpolationAndPerspective(
        ICanvasBitmap* bitmap, 
        float x,
        float y,
        Rect sourceRectangle,
        float opacity,
        CanvasImageInterpolation interpolation,
        Matrix4x4 perspective)
    {
        auto offset = Vector2{ x, y };
        return DrawBitmapImpl(bitmap, &offset, nullptr, &sourceRectangle, opacity, interpolation, &perspective);
    }

    IFACEMETHODIMP CanvasDrawingSession::DrawImageToRectWithSourceRectAndOpacityAndInterpolationAndPerspective(
        ICanvasBitmap* bitmap,
        Rect destinationRectangle,
        Rect sourceRectangle,
        float opacity,
        CanvasImageInterpolation interpolation,
        Matrix4x4 perspective)
    {
        return DrawBitmapImpl(bitmap, nullptr, &destinationRectangle, &sourceRectangle, opacity, interpolation, &perspective);
    }


    class DrawImageWorker
    {
        ICanvasDevice* m_canvasDevice;
        ID2D1DeviceContext1* m_deviceContext;
        Vector2* m_offset;
        Rect* m_destinationRect;
        Rect* m_sourceRect;
        float m_opacity;
        CanvasImageInterpolation m_interpolation;

        D2D1_RECT_F m_d2dSourceRect;
        ComPtr<ID2D1Image> m_opacityEffectOutput;
        ComPtr<ID2D1Image> m_borderEffectOutput;

    public:
        DrawImageWorker(ICanvasDevice* canvasDevice, ID2D1DeviceContext1* deviceContext, Vector2* offset, Rect* destinationRect, Rect* sourceRect, float opacity, CanvasImageInterpolation interpolation)
            : m_canvasDevice(canvasDevice)
            , m_deviceContext(deviceContext)
            , m_offset(offset)
            , m_destinationRect(destinationRect)
            , m_sourceRect(sourceRect)
            , m_opacity(opacity)
            , m_interpolation(interpolation)
        {
            assert(m_offset || m_destinationRect);

            if (m_sourceRect)
                m_d2dSourceRect = ToD2DRect(*sourceRect);
        }

        void DrawBitmap(ICanvasBitmap* bitmap, Numerics::Matrix4x4* perspective)
        {
            DrawBitmap(As<ICanvasBitmapInternal>(bitmap).Get(), perspective);
        }

        void DrawImage(ICanvasImage* image, CanvasComposite const* composite)
        {
            // If this is a bitmap being drawn with sufficiently simple options, we can take the DrawBitmap fast path.
            auto internalBitmap = MaybeAs<ICanvasBitmapInternal>(image);
            
            if (internalBitmap &&
                IsValidDrawBitmapCompositeMode(composite) &&
                IsValidDrawBitmapInterpolationMode())
            {
                DrawBitmap(internalBitmap.Get(), nullptr);
            }
            else
            {
                // If DrawBitmap cannot handle this request, we must use the DrawImage slow path.

                auto internalImage = As<ICanvasImageInternal>(image);
                auto d2dImage = internalImage->GetD2DImage(m_canvasDevice, m_deviceContext);

                auto d2dInterpolationMode = static_cast<D2D1_INTERPOLATION_MODE>(m_interpolation);
                auto d2dCompositeMode = composite ? static_cast<D2D1_COMPOSITE_MODE>(*composite)
                                                  : GetCompositeModeFromPrimitiveBlend();

                if (m_offset)
                    DrawImageAtOffset(d2dImage.Get(), *m_offset, d2dInterpolationMode, d2dCompositeMode);
                else
                    DrawImageToRect(d2dImage.Get(), *m_destinationRect, d2dInterpolationMode, d2dCompositeMode);
            }
        }

    private:
        void DrawBitmap(ICanvasBitmapInternal* internalBitmap, Numerics::Matrix4x4* perspective)
        {
            auto& d2dBitmap = internalBitmap->GetD2DBitmap();

            auto d2dDestRect = CalculateDestRect(d2dBitmap.Get());

            m_deviceContext->DrawBitmap(
                d2dBitmap.Get(),
                &d2dDestRect,
                m_opacity,
                static_cast<D2D1_INTERPOLATION_MODE>(m_interpolation),
                GetD2DSourceRect(),
                ReinterpretAs<D2D1_MATRIX_4X4_F*>(perspective));
        }

        void DrawImageAtOffset(
            ID2D1Image* d2dImage,
            Vector2 offset,
            D2D1_INTERPOLATION_MODE d2dInterpolationMode,
            D2D1_COMPOSITE_MODE d2dCompositeMode)
        {
            auto d2dOffset = ToD2DPoint(offset);

            d2dImage = MaybeApplyOpacityEffect(d2dImage);

            m_deviceContext->DrawImage(d2dImage, &d2dOffset, GetD2DSourceRect(), d2dInterpolationMode, d2dCompositeMode);
        }

        void DrawImageToRect(
            ID2D1Image* d2dImage,
            Rect const& destinationRect,
            D2D1_INTERPOLATION_MODE d2dInterpolationMode,
            D2D1_COMPOSITE_MODE d2dCompositeMode)
        {
            assert(m_sourceRect);

            d2dImage = MaybeAdjustD2DSourceRect(d2dImage);
            d2dImage = MaybeApplyOpacityEffect(d2dImage);

            float sourceWidth  = m_d2dSourceRect.right - m_d2dSourceRect.left;
            float sourceHeight = m_d2dSourceRect.bottom - m_d2dSourceRect.top;

            if (sourceWidth == 0.0f || sourceHeight == 0.0f)
            {
                // There's no useful scale factor for scaling from something
                // that is zero sized. Consistent with observed DrawBitmap
                // behavior, we don't attempt to draw anything in this case.
                return;
            }

            auto offset = Vector2{ destinationRect.X, destinationRect.Y };
            auto scale = Vector2{ destinationRect.Width / sourceWidth, destinationRect.Height / sourceHeight };

            TemporaryTransform<ID2D1DeviceContext1> transform(m_deviceContext, offset, scale);

            auto d2dOffset = D2D1_POINT_2F{ 0, 0 };
            m_deviceContext->DrawImage(d2dImage, &d2dOffset, &m_d2dSourceRect, d2dInterpolationMode, d2dCompositeMode);
        }

        ID2D1Image* MaybeApplyOpacityEffect(ID2D1Image* d2dImage)
        {
            if (m_opacity >= 1.0f)
                return d2dImage;

            ComPtr<ID2D1Effect> opacityEffect;

            ThrowIfFailed(m_deviceContext->CreateEffect(CLSID_D2D1ColorMatrix, &opacityEffect));

            if (auto bitmap = MaybeAs<ID2D1Bitmap>(d2dImage))
            {
                //
                // When drawing a bitmap we need to explicitly compensate for
                // the bitmap's DPI before passing it to the color matrix effect
                // (since effects by default ignore a bitmap's DPI).
                //
                ThrowIfFailed(D2D1::SetDpiCompensatedEffectInput(m_deviceContext, opacityEffect.Get(), 0, bitmap.Get()));
            }
            else
            {
                opacityEffect->SetInput(0, d2dImage);
            }

            D2D1_MATRIX_5X4_F opacityMatrix = D2D1::Matrix5x4F(
                1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, m_opacity,
                0, 0, 0, 0);

            opacityEffect->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, opacityMatrix);
            
            opacityEffect->GetOutput(&m_opacityEffectOutput);
            return m_opacityEffectOutput.Get();
        }

        // DrawBitmap uses the current primitive blend setting, but DrawImage
        // takes an explicit composite mode parameter. We can only substitute
        // the former for the latter if these match.
        //
        // In some cases where they do not match, we could change the primitive
        // blend, use DrawBitmap, then change it back again. But that would be
        // more intrusive, so this implementation plays it safe and only
        // optimizes the simple case where the modes match exactly.
        //
        // If the composite parameter is null, the caller did not explicitly
        // specify a composite mode.  In that case we will use
        // GetCompositeModeFromPrimitiveBlend, so any primitive blend that has a
        // matching composite mode is valid.
        bool IsValidDrawBitmapCompositeMode(CanvasComposite const* composite)
        {
            switch (m_deviceContext->GetPrimitiveBlend())
            {
            case D2D1_PRIMITIVE_BLEND_SOURCE_OVER:
                return !composite || *composite == CanvasComposite::SourceOver;
                
            case D2D1_PRIMITIVE_BLEND_COPY:
                return !composite || *composite == CanvasComposite::Copy;
                
            case D2D1_PRIMITIVE_BLEND_ADD:
                return !composite || *composite == CanvasComposite::Add;
                
            default:
                return false;
            }
        }

        // When using a DrawImage overload that does not take an explicit
        // composite mode parameter, we try to match the current device context
        // primitive blend setting.
        D2D1_COMPOSITE_MODE GetCompositeModeFromPrimitiveBlend()
        {
            switch (m_deviceContext->GetPrimitiveBlend())
            {
            case D2D1_PRIMITIVE_BLEND_SOURCE_OVER:
                return D2D1_COMPOSITE_MODE_SOURCE_OVER;
                
            case D2D1_PRIMITIVE_BLEND_COPY:
                return D2D1_COMPOSITE_MODE_SOURCE_COPY;
                
            case D2D1_PRIMITIVE_BLEND_ADD:
                return D2D1_COMPOSITE_MODE_PLUS;
                
            case D2D1_PRIMITIVE_BLEND_MIN:
                ThrowHR(E_FAIL, Strings::DrawImageMinBlendNotSupported);
                
            default:
                ThrowHR(E_UNEXPECTED);
            }
        }
    
        // Although there are some ID2D1DeviceContext::DrawBitmap methods that
        // appear to take a full set of interpolation modes, it turns out that
        // the implementation of these do not all fully match the behavior of
        // their DrawImage equivalents.  Therefore, we will only use DrawBitmap
        // for this limited set of interpolation modes.
        bool IsValidDrawBitmapInterpolationMode()
        {
            return m_interpolation == CanvasImageInterpolation::Linear ||
                   m_interpolation == CanvasImageInterpolation::NearestNeighbor;
        }

        // DrawImage infers output size from the source image, but DrawBitmap
        // takes an explicit dest rect.  So to use DrawBitmap, we must duplicate
        // the same size logic that DrawImage would normally apply for us.
        D2D1_RECT_F CalculateDestRect(ID2D1Bitmap* d2dBitmap)
        {
            if (m_destinationRect)
                return ToD2DRect(*m_destinationRect);
            
            D2D1_SIZE_F destSize;
            
            if (m_sourceRect)
            {
                // If there is an explicit source rectangle, that determines the destination size too.
                destSize = D2D1_SIZE_F{ m_sourceRect->Width, m_sourceRect->Height };
            }
            else
            {
                destSize = GetBitmapSize(m_deviceContext->GetUnitMode(), d2dBitmap);
            }
            
            return D2D1_RECT_F
            {
                m_offset->X,
                m_offset->Y,
                m_offset->X + destSize.width,
                m_offset->Y + destSize.height
            };
        }

        ID2D1Image* MaybeAdjustD2DSourceRect(ID2D1Image* d2dImage)
        {
            auto d2dBitmap = MaybeAs<ID2D1Bitmap>(d2dImage);
            if (!d2dBitmap)
                return d2dImage;

            // If this is actually a bitmap, then sourceRect needs to be
            // adjusted so that it doesn't go beyond the bounds of the image.
            // This is in keeping with DrawBitmap's behavior.  We don't attempt
            // to do this with more generic image types since this is
            // prohibitively expensive.

            m_d2dSourceRect.left = std::max(m_d2dSourceRect.left, 0.0f); 
            m_d2dSourceRect.top  = std::max(m_d2dSourceRect.top,  0.0f);

            D2D1_SIZE_F size = GetBitmapSize(m_deviceContext->GetUnitMode(), d2dBitmap.Get());

            m_d2dSourceRect.right  = std::min(m_d2dSourceRect.right,  size.width);
            m_d2dSourceRect.bottom = std::min(m_d2dSourceRect.bottom, size.height);

            // D2D bitmap and image rendering paths have different border sampling behavior, so
            // when we emulate DrawBitmap using DrawImage, we must insert an explicit BorderEffect
            // to avoid unwanted translucency along the edges. We could get fancy and only do this
            // if the source rect is such that we will actually sample outside the bounds of the
            // image, but it is non trivial to detect that for different filter modes, and this
            // is a slow path in any case so we keep it simple and always add the border.

            ComPtr<ID2D1Effect> borderEffect;
            ThrowIfFailed(m_deviceContext->CreateEffect(CLSID_D2D1Border, &borderEffect));
            ThrowIfFailed(D2D1::SetDpiCompensatedEffectInput(m_deviceContext, borderEffect.Get(), 0, d2dBitmap.Get()));

            borderEffect->GetOutput(&m_borderEffectOutput);
            return m_borderEffectOutput.Get();
        }

        D2D1_RECT_F* GetD2DSourceRect()
        {
            if (m_sourceRect)
                return &m_d2dSourceRect;
            else
                return nullptr;
        }
    };

    HRESULT CanvasDrawingSession::DrawImageImpl(
        ICanvasImage* image,
        Vector2* offset,
        Rect* destinationRect,
        Rect* sourceRect,
        float opacity,
        CanvasImageInterpolation interpolation,
        CanvasComposite const* composite)
    {
        return ExceptionBoundary([&]
        {
            auto& deviceContext = GetResource();
            CheckInPointer(image);

            DrawImageWorker(GetDevice().Get(), deviceContext.Get(), offset, destinationRect, sourceRect, opacity, interpolation).DrawImage(image, composite);
        });

    }

    HRESULT CanvasDrawingSession::DrawBitmapImpl(
        ICanvasBitmap* bitmap,
        Vector2* offset,
        Rect* destinationRect,
        Rect* sourceRect,
        float opacity,
        CanvasImageInterpolation interpolation,
        Numerics::Matrix4x4* perspective)
    {        
        return ExceptionBoundary([&]
        {
            auto& deviceContext = GetResource();
            CheckInPointer(bitmap);

            DrawImageWorker(GetDevice().Get(), deviceContext.Get(), offset, destinationRect, sourceRect, opacity, interpolation).DrawBitmap(bitmap, perspective);
        });
    }


    static bool ArePointsInsideBitmap(ID2D1Bitmap* bitmap, D2D1_POINT_2F const& point1, D2D1_POINT_2F const& point2, D2D1_UNIT_MODE unitMode)
    {
        D2D1_SIZE_F bitmapSize = GetBitmapSize(unitMode, bitmap);

        const float epsilon = 0.001f;

        return point1.x >= -epsilon &&
               point1.y >= -epsilon &&
               point2.x >= -epsilon &&
               point2.y >= -epsilon &&
               point1.x <= bitmapSize.width  + epsilon &&
               point1.y <= bitmapSize.height + epsilon &&
               point2.x <= bitmapSize.width  + epsilon &&
               point2.y <= bitmapSize.height + epsilon;
    }


    static bool TryGetFillOpacityMaskParameters(ID2D1Brush* opacityBrush, ID2D1DeviceContext1* deviceContext, D2D1_RECT_F const& destRect, ID2D1Bitmap** opacityBitmap, D2D1_RECT_F* opacitySourceRect)
    {
        // Is this a bitmap brush?
        auto bitmapBrush = MaybeAs<ID2D1BitmapBrush1>(opacityBrush);

        if (!bitmapBrush)
            return false;

        bitmapBrush->GetBitmap(opacityBitmap);
        
        if (!*opacityBitmap)
            return false;

        // Make sure the brush transform contains only positive scaling and translation, as other
        // transforms cannot be represented in FillOpacityMask sourceRect/destRect format.
        D2D1::Matrix3x2F brushTransform;
        bitmapBrush->GetTransform(&brushTransform);

        if (brushTransform._11 <= 0 ||
            brushTransform._22 <= 0 ||
            brushTransform._12 != 0 ||
            brushTransform._21 != 0)
        {
            return false;
        }

        // Transform the dest rect by the inverse of the brush transform, yielding a FillOpacityMask source rect.
        if (!D2D1InvertMatrix(&brushTransform))
            return false;

        auto tl = D2D1_POINT_2F{ destRect.left,  destRect.top    } * brushTransform;
        auto br = D2D1_POINT_2F{ destRect.right, destRect.bottom } * brushTransform;

        // Can't use FillOpacityMask if the source rect goes outside the bounds of the bitmap.
        if (!ArePointsInsideBitmap(*opacityBitmap, tl, br, deviceContext->GetUnitMode()))
            return false;

        // FillOpacityMask always uses default alpha and interpolation mode.
        if (bitmapBrush->GetOpacity() != 1.0f)
            return false;

        if (bitmapBrush->GetInterpolationMode1() != D2D1_BITMAP_INTERPOLATION_MODE_LINEAR)
            return false;

        // FillOpacityMask requires that antialiasing be disabled.
        if (deviceContext->GetAntialiasMode() != D2D1_ANTIALIAS_MODE_ALIASED)
            return false;

        // Ok then! FillOpacityMask is a go.
        *opacitySourceRect = D2D1_RECT_F{ tl.x, tl.y, br.x, br.y };

        return true;
    }

#if WINVER > _WIN32_WINNT_WINBLUE
    IFACEMETHODIMP CanvasDrawingSession::DrawInk(IIterable<InkStroke*>* inkStrokeCollection)
    {
        return ExceptionBoundary(
            [&]
            {
                DrawInkImpl(inkStrokeCollection, InkAdapter::GetInstance()->IsHighContrastEnabled());
            });
    }

    IFACEMETHODIMP CanvasDrawingSession::DrawInkWithHighContrast(
        IIterable<InkStroke*>* inkStrokeCollection,
        boolean highContrast)
    {
        return ExceptionBoundary(
            [&]
            {
                DrawInkImpl(inkStrokeCollection, !!highContrast);
            });
    }

    void CanvasDrawingSession::DrawInkImpl(
        IIterable<InkStroke*>* inkStrokeCollection,
        bool highContrast)
    {
        auto& deviceContext = GetResource();

        CheckInPointer(inkStrokeCollection);

        ComPtr<IUnknown> inkStrokeCollectionAsIUnknown = As<IUnknown>(inkStrokeCollection);

        if (!m_inkD2DRenderer)
        {
            m_inkD2DRenderer = InkAdapter::GetInstance()->CreateInkRenderer();
        }

        if (!m_inkStateBlock)
        {
            ComPtr<ID2D1Factory> d2dFactory;
            deviceContext->GetFactory(&d2dFactory);

            ThrowIfFailed(As<ID2D1Factory1>(d2dFactory)->CreateDrawingStateBlock(&m_inkStateBlock));
        }

        deviceContext->SaveDrawingState(m_inkStateBlock.Get());

        auto restoreStateWarden = MakeScopeWarden([&] { deviceContext->RestoreDrawingState(m_inkStateBlock.Get()); });

        ThrowIfFailed(m_inkD2DRenderer->Draw(
            deviceContext.Get(), 
            inkStrokeCollectionAsIUnknown.Get(), 
            highContrast));
    }

#endif

    ID2D1SolidColorBrush* CanvasDrawingSession::GetColorBrush(Color const& color)
    {
        if (m_solidColorBrush)
        {
            m_solidColorBrush->SetColor(ToD2DColor(color));
        }
        else
        {
            auto& deviceContext = GetResource();
            ThrowIfFailed(deviceContext->CreateSolidColorBrush(ToD2DColor(color), &m_solidColorBrush));
        }

        return m_solidColorBrush.Get();
    }


    ComPtr<ID2D1Brush> CanvasDrawingSession::ToD2DBrush(ICanvasBrush* brush)
    {
        if (!brush)
            return nullptr;

        auto& deviceContext = GetResource();

        return As<ICanvasBrushInternal>(brush)->GetD2DBrush(deviceContext.Get(), GetBrushFlags::None);
    }


    //
    // Converts the given offset from DIPs to the appropriate unit
    //
    static D2D1_POINT_2F GetOffsetInCorrectUnits(ID2D1DeviceContext1* deviceContext, D2D1_POINT_2F const& offset)
    {
        auto unitMode = deviceContext->GetUnitMode();

        if (unitMode == D2D1_UNIT_MODE_DIPS)
        {
            return offset;
        }
        else if (unitMode == D2D1_UNIT_MODE_PIXELS)
        {
            auto dpi = GetDpi(deviceContext);
            D2D1_POINT_2F adjustedOffset = {
                (float)DipsToPixels(offset.x, dpi, CanvasDpiRounding::Floor),
                (float)DipsToPixels(offset.y, dpi, CanvasDpiRounding::Floor)
            };
            return adjustedOffset;
        }
        else
        {
            assert(false);
            ThrowHR(E_UNEXPECTED);
        }
    }

    //
    // Gets the current transform from the given device context, stripping out
    // the current offset
    //
    static Matrix3x2 GetTransform(ID2D1DeviceContext1* deviceContext, D2D1_POINT_2F const& offset)
    {
        D2D1_MATRIX_3X2_F transform;
        deviceContext->GetTransform(&transform);

        // We assume that the currently set transform has the offset applied to
        // it, correctly set for the current unit mode.  We need to subtract
        // that from the transform before returning it.
        auto adjustedOffset = GetOffsetInCorrectUnits(deviceContext, offset);
        transform._31 -= adjustedOffset.x;
        transform._32 -= adjustedOffset.y;

        return *reinterpret_cast<ABI::Microsoft::Graphics::Canvas::Numerics::Matrix3x2*>(&transform);
    }

    //
    // Sets the transform on the given device context, applied the offset.
    //
    static void SetTransform(ID2D1DeviceContext1* deviceContext, D2D1_POINT_2F const& offset, Matrix3x2 const& matrix)
    {
        auto adjustedOffset = GetOffsetInCorrectUnits(deviceContext, offset);

        D2D1_MATRIX_3X2_F transform = *(ReinterpretAs<D2D1_MATRIX_3X2_F const*>(&matrix));
        transform._31 += adjustedOffset.x;
        transform._32 += adjustedOffset.y;

        deviceContext->SetTransform(transform);
    }

    IFACEMETHODIMP CanvasDrawingSession::get_Transform(ABI::Microsoft::Graphics::Canvas::Numerics::Matrix3x2* value)
    {
        return ExceptionBoundary(
            [&]
            {                               
                auto& deviceContext = GetResource();
                CheckInPointer(value);

                *value = GetTransform(deviceContext.Get(), m_offset);
            });
    }

    IFACEMETHODIMP CanvasDrawingSession::put_Transform(ABI::Microsoft::Graphics::Canvas::Numerics::Matrix3x2 value)
    {
        return ExceptionBoundary(
            [&]
            {
                auto& deviceContext = GetResource();

                SetTransform(deviceContext.Get(), m_offset, value);
            });
    }

    IFACEMETHODIMP CanvasDrawingSession::get_Units(CanvasUnits* value)
    {
        return ExceptionBoundary(
            [&]
            {
                auto& deviceContext = GetResource();
                CheckInPointer(value);

                *value = static_cast<CanvasUnits>(deviceContext->GetUnitMode());
            });
    }

    IFACEMETHODIMP CanvasDrawingSession::put_Units(CanvasUnits value)
    {
        return ExceptionBoundary(
            [&]
            {
                auto& deviceContext = GetResource();

                if (m_offset.x != 0 || m_offset.y != 0)
                {
                    auto transform = GetTransform(deviceContext.Get(), m_offset);
                    deviceContext->SetUnitMode(static_cast<D2D1_UNIT_MODE>(value));
                    SetTransform(deviceContext.Get(), m_offset, transform);
                }
                else
                {
                    deviceContext->SetUnitMode(static_cast<D2D1_UNIT_MODE>(value));
                }
            });
    }

    IFACEMETHODIMP CanvasDrawingSession::get_EffectBufferPrecision(IReference<CanvasBufferPrecision>** value)
    {
        return ExceptionBoundary(
            [&]
            {
                auto& deviceContext = GetResource();
                CheckAndClearOutPointer(value);

                D2D1_RENDERING_CONTROLS renderingControls;
                deviceContext->GetRenderingControls(&renderingControls);

                // If the value is not unknown, box it as an IReference.
                // Unknown precision returns null.
                if (renderingControls.bufferPrecision != D2D1_BUFFER_PRECISION_UNKNOWN)
                {
                    auto nullable = Make<Nullable<CanvasBufferPrecision>>(FromD2DBufferPrecision(renderingControls.bufferPrecision));
                    CheckMakeResult(nullable);

                    ThrowIfFailed(nullable.CopyTo(value));
                }
            });
    }

    IFACEMETHODIMP CanvasDrawingSession::put_EffectBufferPrecision(IReference<CanvasBufferPrecision>* value)
    {
        return ExceptionBoundary(
            [&]
            {
                auto& deviceContext = GetResource();

                D2D1_RENDERING_CONTROLS renderingControls;
                deviceContext->GetRenderingControls(&renderingControls);

                if (value)
                {
                    // Convert non-null values from Win2D to D2D format.
                    CanvasBufferPrecision bufferPrecision;
                    ThrowIfFailed(value->get_Value(&bufferPrecision));

                    renderingControls.bufferPrecision = ToD2DBufferPrecision(bufferPrecision);
                }
                else
                {
                    // Null references -> unknown.
                    renderingControls.bufferPrecision = D2D1_BUFFER_PRECISION_UNKNOWN;
                }

                deviceContext->SetRenderingControls(&renderingControls);
            });
    }

    IFACEMETHODIMP CanvasDrawingSession::get_EffectTileSize(BitmapSize* value)
    {
        return ExceptionBoundary(
            [&]
            {
                auto& deviceContext = GetResource();
                CheckInPointer(value);

                D2D1_RENDERING_CONTROLS renderingControls;
                deviceContext->GetRenderingControls(&renderingControls);

                *value = BitmapSize{ renderingControls.tileSize.width, renderingControls.tileSize.height };
            });
    }

    IFACEMETHODIMP CanvasDrawingSession::put_EffectTileSize(BitmapSize value)
    {
        return ExceptionBoundary(
            [&]
            {
                auto& deviceContext = GetResource();

                D2D1_RENDERING_CONTROLS renderingControls;
                deviceContext->GetRenderingControls(&renderingControls);

                renderingControls.tileSize = D2D_SIZE_U{ value.Width, value.Height };

                deviceContext->SetRenderingControls(&renderingControls);
            });
    }


    IFACEMETHODIMP CanvasDrawingSession::get_Device(ICanvasDevice** value)
    {
        using namespace ::Microsoft::WRL::Wrappers;

        return ExceptionBoundary(
            [&]
            {
                CheckInPointer(value);

                ThrowIfFailed(GetDevice().CopyTo(value));
            });
    }

    ComPtr<ICanvasDevice> const& CanvasDrawingSession::GetDevice()
    {
        if (!m_owner)
        {
            auto& deviceContext = GetResource();

            ComPtr<ID2D1Device> d2dDevice;
            deviceContext->GetDevice(&d2dDevice);

            m_owner = ResourceManager::GetOrCreate<ICanvasDevice>(d2dDevice.Get());
        }

        return m_owner;
    }

    IFACEMETHODIMP CanvasDrawingSession::get_Dpi(float* dpi)
    {
        return ExceptionBoundary(
            [&]
            {
                auto& deviceContext = GetResource();
                CheckInPointer(dpi);

                *dpi = GetDpi(deviceContext);
            });
    }

    IFACEMETHODIMP CanvasDrawingSession::ConvertPixelsToDips(int pixels, float* dips)
    {
        return ExceptionBoundary(
            [&]
            {
                auto& deviceContext = GetResource();
                CheckInPointer(dips);

                *dips = PixelsToDips(pixels, GetDpi(deviceContext));
            });
    }

    IFACEMETHODIMP CanvasDrawingSession::ConvertDipsToPixels(float dips, CanvasDpiRounding dpiRounding, int* pixels)
    {
        return ExceptionBoundary(
            [&]
            {
                auto& deviceContext = GetResource();
                CheckInPointer(pixels);

                *pixels = DipsToPixels(dips, GetDpi(deviceContext), dpiRounding);
            });
    }


    //
    // CreateLayer
    //

    IFACEMETHODIMP CanvasDrawingSession::CreateLayerWithOpacity(
        float opacity,
        ICanvasActiveLayer** layer)
    {
        return CreateLayerImpl(opacity, nullptr, nullptr, nullptr, nullptr, CanvasLayerOptions::None, layer);
    }

    IFACEMETHODIMP CanvasDrawingSession::CreateLayerWithOpacityAndClipRectangle(
        float opacity,
        Rect clipRectangle,
        ICanvasActiveLayer** layer)
    {
        return CreateLayerImpl(opacity, nullptr, &clipRectangle, nullptr, nullptr, CanvasLayerOptions::None, layer);
    }

    // Returns true if the current transform matrix contains only scaling and translation, but no rotation or skew.
    static bool TransformIsAxisPreserving(ID2D1DeviceContext* deviceContext)
    {
        D2D1_MATRIX_3X2_F transform;

        deviceContext->GetTransform(&transform);

        return transform._12 == 0.0f &&
               transform._21 == 0.0f;
    }

    HRESULT CanvasDrawingSession::CreateLayerImpl(
        float opacity,
        ICanvasBrush* opacityBrush,
        Rect const* clipRectangle,
        ICanvasGeometry* clipGeometry,
        Matrix3x2 const* geometryTransform,
        CanvasLayerOptions options,
        ICanvasActiveLayer** layer)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckAndClearOutPointer(layer);

                auto& deviceContext = GetResource();

                // Convert the layer parameters to D2D format.
                auto d2dBrush = ToD2DBrush(opacityBrush);
                auto d2dRect = clipRectangle ? ToD2DRect(*clipRectangle) : D2D1::InfiniteRect();
                auto d2dGeometry = clipGeometry ? GetWrappedResource<ID2D1Geometry>(clipGeometry) : nullptr;
                auto d2dMatrix = geometryTransform ? *ReinterpretAs<D2D1_MATRIX_3X2_F const*>(geometryTransform) : D2D1::Matrix3x2F::Identity();
                auto d2dAntialiasMode = deviceContext->GetAntialiasMode();

                // Simple cases can be optimized to use PushAxisAlignedClip instead of PushLayer.
                bool isAxisAlignedClip = clipRectangle &&
                                         !d2dBrush &&
                                         !d2dGeometry &&
                                         opacity == 1.0f &&
                                         options == CanvasLayerOptions::None &&
                                         TransformIsAxisPreserving(deviceContext.Get());

                // Store a unique ID, used for validation in PopLayer. This extra state 
                // is needed because the D2D PopLayer method always just pops the topmost 
                // layer, but we want to make sure our CanvasActiveLayer objects are 
                // closed in the right order if there is nesting.
                //
                // Unlike most places where we stash extra state in a resource wrapper 
                // type, this does not break interop, because our IClosable based layer 
                // API already prevents cross-API push and pop of layers. You can do 
                // interop in code using layers, but cannot push from one side of the 
                // interop boundary and then pop from the other, which is what would 
                // break this tracking were it possible.

                int layerId = ++m_nextLayerId;

                m_activeLayerIds.push_back(layerId);

                // Construct a scope object that will pop the layer when its Close method is called.
                WeakRef weakSelf = AsWeak(this);

                auto activeLayer = Make<CanvasActiveLayer>(
                    [weakSelf, layerId, isAxisAlignedClip]() mutable
                    {
                        auto strongSelf = LockWeakRef<ICanvasDrawingSession>(weakSelf);
                        auto self = static_cast<CanvasDrawingSession*>(strongSelf.Get());

                        if (self)
                            self->PopLayer(layerId, isAxisAlignedClip);
                    });

                CheckMakeResult(activeLayer);

                if (isAxisAlignedClip)
                {
                    // Tell D2D to push an axis aligned clip region.
                    deviceContext->PushAxisAlignedClip(&d2dRect, d2dAntialiasMode);
                }
                else
                {
                    // Tell D2D to push the layer.
                    D2D1_LAYER_PARAMETERS1 parameters =
                    {
                        d2dRect,
                        d2dGeometry.Get(),
                        d2dAntialiasMode,
                        d2dMatrix,
                        opacity,
                        d2dBrush.Get(),
                        static_cast<D2D1_LAYER_OPTIONS1>(options)
                    };

                    deviceContext->PushLayer(&parameters, nullptr);
                }

                ThrowIfFailed(activeLayer.CopyTo(layer));
            });
    }

    void CanvasDrawingSession::PopLayer(int layerId, bool isAxisAlignedClip)
    {
        auto& deviceContext = GetResource();

        assert(!m_activeLayerIds.empty());

        if (m_activeLayerIds.back() != layerId)
            ThrowHR(E_FAIL, Strings::PoppedWrongLayer);

        m_activeLayerIds.pop_back();

        if (isAxisAlignedClip)
        {
            deviceContext->PopAxisAlignedClip();
        }
        else
        {
            deviceContext->PopLayer();
        }
    }

}}}}
