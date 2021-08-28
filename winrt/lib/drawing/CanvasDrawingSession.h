// Copyright (c) Microsoft Corporation. All rights reserved.
//
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.

#pragma once

namespace ABI { namespace Microsoft { namespace Graphics { namespace Canvas
{
    using namespace ABI::Microsoft::Graphics::Canvas::Geometry;
    using namespace ABI::Microsoft::Graphics::Canvas::Numerics;
    using namespace ABI::Windows::Foundation;

#if WINVER > _WIN32_WINNT_WINBLUE
    using namespace ABI::Windows::UI::Input::Inking;
    using namespace ABI::Windows::UI::ViewManagement;
    using namespace ABI::Microsoft::Graphics::Canvas::Svg;
#endif

    using namespace ::Microsoft::WRL;

    class ICanvasDrawingSessionAdapter
    {
    public:
        virtual ~ICanvasDrawingSessionAdapter() = default;

        virtual void EndDraw(ID2D1DeviceContext1* d2dDeviceContext) = 0;
    };

#if WINVER > _WIN32_WINNT_WINBLUE
    class DefaultInkAdapter;

    class InkAdapter : public Singleton<InkAdapter, DefaultInkAdapter>
    {
    public:
        virtual ~InkAdapter() = default;

        virtual ComPtr<IInkD2DRenderer> CreateInkRenderer() = 0;
        virtual bool IsHighContrastEnabled() = 0;
    };

    class DefaultInkAdapter : public InkAdapter
    {
        ComPtr<IAccessibilitySettings> m_accessibilitySettings;

    public:
        virtual ComPtr<IInkD2DRenderer> CreateInkRenderer() override;
        virtual bool IsHighContrastEnabled() override;
    };
#endif

    class CanvasDrawingSession : RESOURCE_WRAPPER_RUNTIME_CLASS(
        ID2D1DeviceContext1,
        CanvasDrawingSession,
        ICanvasDrawingSession,
        ICanvasResourceCreatorWithDpi,
        ICanvasResourceCreator)
    {
        InspectableClass(RuntimeClass_Microsoft_Graphics_Canvas_CanvasDrawingSession, BaseTrust);

        std::shared_ptr<ICanvasDrawingSessionAdapter> m_adapter;
        std::shared_ptr<bool> m_targetHasActiveDrawingSession;
        D2D1_POINT_2F const m_offset;
        
        ComPtr<ID2D1SolidColorBrush> m_solidColorBrush;
        ComPtr<ICanvasTextFormat> m_defaultTextFormat;

        std::vector<int> m_activeLayerIds;
        int m_nextLayerId;

        //
        // Contract:
        //     Drawing sessions created conventionally initialize this member.
        //     Drawing sessions created through interop set this member to null.
        //
        //     The thing this affects is DrawingSession's use as an ICanvasResourceCreator.
        //     If the backpointer is initialized, that is the resource creator's device.
        //     If the backpointer is null, a CanvasDevice wrapper is produced based on 
        //     this drawing session's device context. That wrapper is created on demand 
        //     by get_Device.
        //
        ComPtr<ICanvasDevice> m_owner;

#if WINVER > _WIN32_WINNT_WINBLUE
        ComPtr<IInkD2DRenderer> m_inkD2DRenderer;
        ComPtr<ID2D1DrawingStateBlock1> m_inkStateBlock;
#endif

    public:
        static ComPtr<CanvasDrawingSession> CreateNew(
            ID2D1DeviceContext1* deviceContext,
            std::shared_ptr<ICanvasDrawingSessionAdapter> drawingSessionAdapter,
            ICanvasDevice* owner = nullptr,
            std::shared_ptr<bool> targetHasActiveDrawingSession = nullptr,
            D2D1_POINT_2F offset = D2D1_POINT_2F{ 0, 0 });

        CanvasDrawingSession(
            ID2D1DeviceContext1* deviceContext,
            std::shared_ptr<ICanvasDrawingSessionAdapter> drawingSessionAdapter = nullptr,
            ICanvasDevice* owner = nullptr,
            std::shared_ptr<bool> targetHasActiveDrawingSession = nullptr,
            D2D1_POINT_2F offset = D2D1_POINT_2F{ 0, 0 });


        virtual ~CanvasDrawingSession();

        // IClosable

        IFACEMETHOD(Close)() override;

        // ICanvasDrawingSession

        IFACEMETHOD(Clear)(
            ABI::Windows::UI::Color color) override;

        IFACEMETHOD(ClearHdr)(
            Vector4 colorHdr) override;

        IFACEMETHOD(Flush)() override;

        // 
        // DrawImage
        // 

        static float DefaultDrawImageOpacity() { return 1.0f; }
        static CanvasImageInterpolation DefaultDrawImageInterpolation() { return CanvasImageInterpolation::Linear; }
        // Default value for Composite comes from GetCompositeModeFromPrimitiveBlend

        IFACEMETHOD(DrawImageAtOrigin)(
            ICanvasImage* image) override;

        IFACEMETHOD(DrawImageAtOffset)(
            ICanvasImage* image, 
            Vector2 offset) override;

        IFACEMETHOD(DrawImageAtCoords)(
            ICanvasImage* image, 
            float x,
            float y) override;

        IFACEMETHOD(DrawImageToRect)(
            ICanvasBitmap* bitmap, 
            Rect destinationRectangle) override;

        IFACEMETHOD(DrawImageAtOffsetWithSourceRect)(
            ICanvasImage* image, 
            Vector2 offset,
            Rect sourceRectangle) override;

        IFACEMETHOD(DrawImageAtCoordsWithSourceRect)(
            ICanvasImage* image, 
            float x,
            float y,
            Rect sourceRectangle) override;

        IFACEMETHOD(DrawImageToRectWithSourceRect)(
            ICanvasImage* image,
            Rect destinationRectangle,
            Rect sourceRectangle) override;

        IFACEMETHOD(DrawImageAtOffsetWithSourceRectAndOpacity)(
            ICanvasImage* image, 
            Vector2 offset,
            Rect sourceRectangle,
            float opacity) override;

        IFACEMETHOD(DrawImageAtCoordsWithSourceRectAndOpacity)(
            ICanvasImage* image, 
            float x,
            float y,
            Rect sourceRectangle,
            float opacity) override;

        IFACEMETHOD(DrawImageToRectWithSourceRectAndOpacity)(
            ICanvasImage* image,
            Rect destinationRectangle,
            Rect sourceRectangle,
            float opacity) override;

        IFACEMETHOD(DrawImageAtOffsetWithSourceRectAndOpacityAndInterpolation)(
            ICanvasImage* image, 
            Vector2 offset,
            Rect sourceRectangle,
            float opacity,
            CanvasImageInterpolation interpolation) override;

        IFACEMETHOD(DrawImageAtCoordsWithSourceRectAndOpacityAndInterpolation)(
            ICanvasImage* image, 
            float x,
            float y,
            Rect sourceRectangle,
            float opacity,
            CanvasImageInterpolation interpolation) override;

        IFACEMETHOD(DrawImageToRectWithSourceRectAndOpacityAndInterpolation)(
            ICanvasImage* image,
            Rect destinationRectangle,
            Rect sourceRectangle,
            float opacity,
            CanvasImageInterpolation interpolation) override;

        IFACEMETHOD(DrawImageAtOffsetWithSourceRectAndOpacityAndInterpolationAndComposite)(
            ICanvasImage* image, 
            Vector2 offset,
            Rect sourceRectangle,
            float opacity,
            CanvasImageInterpolation interpolation,
            CanvasComposite composite) override;

        IFACEMETHOD(DrawImageAtCoordsWithSourceRectAndOpacityAndInterpolationAndComposite)(
            ICanvasImage* image, 
            float x,
            float y,
            Rect sourceRectangle,
            float opacity,
            CanvasImageInterpolation interpolation,
            CanvasComposite composite) override;

        IFACEMETHOD(DrawImageToRectWithSourceRectAndOpacityAndInterpolationAndComposite)(
            ICanvasImage* image,
            Rect destinationRectangle,
            Rect sourceRectangle,
            float opacity,
            CanvasImageInterpolation interpolation,
            CanvasComposite composite) override;

        IFACEMETHOD(DrawImageAtOffsetWithSourceRectAndOpacityAndInterpolationAndPerspective)(
            ICanvasBitmap* bitmap, 
            Vector2 offset,
            Rect sourceRectangle,
            float opacity,
            CanvasImageInterpolation interpolation,
            Matrix4x4 perspective) override;

        IFACEMETHOD(DrawImageAtCoordsWithSourceRectAndOpacityAndInterpolationAndPerspective)(
            ICanvasBitmap* bitmap, 
            float x,
            float y,
            Rect sourceRectangle,
            float opacity,
            CanvasImageInterpolation interpolation,
            Matrix4x4 perspective) override;

        IFACEMETHOD(DrawImageToRectWithSourceRectAndOpacityAndInterpolationAndPerspective)(
            ICanvasBitmap* bitmap,
            Rect destinationRectangle,
            Rect sourceRectangle,
            float opacity,
            CanvasImageInterpolation interpolation,
            Matrix4x4 perspective) override;
        

#if WINVER > _WIN32_WINNT_WINBLUE
        //
        // DrawInk
        //
        IFACEMETHOD(DrawInk)(IIterable<InkStroke*>* inkStrokes) override;

        IFACEMETHOD(DrawInkWithHighContrast)(IIterable<InkStroke*>* inkStrokes, boolean highContrast) override;

#endif

        //
        // State properties
        //

        IFACEMETHOD(get_Transform)(ABI::Microsoft::Graphics::Canvas::Numerics::Matrix3x2* value) override;
        IFACEMETHOD(put_Transform)(ABI::Microsoft::Graphics::Canvas::Numerics::Matrix3x2 value) override;

        IFACEMETHOD(get_Units)(CanvasUnits* value) override;
        IFACEMETHOD(put_Units)(CanvasUnits value) override;

        IFACEMETHOD(get_EffectBufferPrecision)(IReference<CanvasBufferPrecision>** value) override;
        IFACEMETHOD(put_EffectBufferPrecision)(IReference<CanvasBufferPrecision>* value) override;

        IFACEMETHOD(get_EffectTileSize)(BitmapSize* value) override;
        IFACEMETHOD(put_EffectTileSize)(BitmapSize value) override;

        //
        // CreateLayer
        //

        IFACEMETHOD(CreateLayerWithOpacity)(
            float opacity,
            ICanvasActiveLayer** layer) override;

        IFACEMETHOD(CreateLayerWithOpacityAndClipRectangle)(
            float opacity,
            Rect clipRectangle,
            ICanvasActiveLayer** layer) override;

        //
        // ICanvasResourceCreator
        //

        IFACEMETHODIMP get_Device(ICanvasDevice** value) override;

        //
        // ICanvasResourceCreatorWithDpi
        //

        IFACEMETHODIMP get_Dpi(float* dpi) override;

        IFACEMETHODIMP ConvertPixelsToDips(int pixels, float* dips) override;
        IFACEMETHODIMP ConvertDipsToPixels(float dips, CanvasDpiRounding dpiRounding, int* pixels) override;

    private:
        void DrawLineImpl(
            Vector2 const& p0,
            Vector2 const& p1,
            ID2D1Brush* brush,
            float strokeWidth,
            ICanvasStrokeStyle* strokeStyle);

        void DrawRectangleImpl(
            Rect const& rect,
            ID2D1Brush* brush,
            float strokeWidth,
            ICanvasStrokeStyle* strokeStyle);

        void FillRectangleImpl(
            Rect const& rect,
            ID2D1Brush* brush);

        void DrawRoundedRectangleImpl(
            Rect const& rect,
            float radiusX,
            float radiusY,
            ID2D1Brush* brush,
            float strokeWidth,
            ICanvasStrokeStyle* strokeStyle);

        void FillRoundedRectangleImpl(
            Rect const& rect,
            float radiusX,
            float radiusY,
            ID2D1Brush* brush);

        void DrawEllipseImpl(
            Vector2 const& centerPoint,
            float radiusX,
            float radiusY,
            ID2D1Brush* brush,
            float strokeWidth,
            ICanvasStrokeStyle* strokeStyle);

        void FillEllipseImpl(
            Vector2 const& centerPoint,
            float radiusX,
            float radiusY,
            ID2D1Brush* brush);

        void DrawTextAtRectImpl(
            HSTRING text,
            Rect const& rect,
            ID2D1Brush* brush,
            ICanvasTextFormat* format);

        void DrawTextAtPointImpl(
            HSTRING text,
            Vector2 const& point,
            ID2D1Brush* brush,
            ICanvasTextFormat* format);

        void DrawTextImpl(
            HSTRING text,
            Rect const& rect,
            ID2D1Brush* brush,
            IDWriteTextFormat* format,
            D2D1_DRAW_TEXT_OPTIONS options);

        ICanvasTextFormat* GetDefaultTextFormat();

        void DrawGeometryImpl(
            ICanvasGeometry* geometry,
            ID2D1Brush* brush,
            float strokeWidth,
            ICanvasStrokeStyle* strokeStyle);

        void FillGeometryImpl(
            ICanvasGeometry* geometry,
            ID2D1Brush* brush,
            ID2D1Brush* opacityBrush);

        void DrawCachedGeometryImpl(
            ICanvasCachedGeometry* cachedGeometry,
            ID2D1Brush* brush);

        ID2D1SolidColorBrush* GetColorBrush(ABI::Windows::UI::Color const& color);
        ComPtr<ID2D1Brush> ToD2DBrush(ICanvasBrush* brush);

        HRESULT DrawImageImpl(
            ICanvasImage* image,
            Vector2* offset,
            Rect* destinationRect,
            Rect* sourceRect,
            float opacity,
            CanvasImageInterpolation interpolation,
            CanvasComposite const* composite);

        HRESULT DrawBitmapImpl(
            ICanvasBitmap* bitmap,
            Vector2* offset,
            Rect* destinationRect,
            Rect* sourceRect,
            float opacity,
            CanvasImageInterpolation interpolation,
            ABI::Microsoft::Graphics::Canvas::Numerics::Matrix4x4* perspective);

        HRESULT CreateLayerImpl(
            float opacity,
            ICanvasBrush* opacityBrush,
            Rect const* clipRectangle,
            ICanvasGeometry* clipGeometry,
            Matrix3x2 const* geometryTransform,
            CanvasLayerOptions options,
            ICanvasActiveLayer** layer);

        void PopLayer(int layerId, bool isAxisAlignedClip);

#if WINVER > _WIN32_WINNT_WINBLUE
        void DrawInkImpl(IIterable<InkStroke*>* inkStrokeCollection, bool highContrast);
#endif

        ComPtr<ICanvasDevice> const& GetDevice();

        static void InitializeDefaultState(ID2D1DeviceContext1* deviceContext);
    };


    //
    // A CanvasDrawingSessionAdapter that calls BeginDraw and EndDraw on the
    // device context.
    //
    class SimpleCanvasDrawingSessionAdapter : public ICanvasDrawingSessionAdapter,
                                              private LifespanTracker<SimpleCanvasDrawingSessionAdapter>
    {
    public:
        SimpleCanvasDrawingSessionAdapter(ID2D1DeviceContext1* d2dDeviceContext)
        {
            d2dDeviceContext->BeginDraw();
        }

        virtual void EndDraw(ID2D1DeviceContext1* d2dDeviceContext) override
        {
            ThrowIfFailed(d2dDeviceContext->EndDraw());
        }
    };
}}}}
