/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2013 CERN
 * @author Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include <wx/wx.h>
#include <wx/frame.h>
#include <wx/window.h>
#include <wx/event.h>
#include <wx/colour.h>
#include <wx/filename.h>

#include <class_drawpanel_gal.h>
#include <view/view.h>
#include <view/wx_view_controls.h>
#include <pcb_painter.h>

#include <gal/graphics_abstraction_layer.h>
#include <gal/opengl/opengl_gal.h>
#include <gal/cairo/cairo_gal.h>

#ifdef __WXDEBUG__
#include <profile.h>
#endif /* __WXDEBUG__ */

#define METRIC_UNIT_LENGTH (1e9)


EDA_DRAW_PANEL_GAL::EDA_DRAW_PANEL_GAL( wxWindow* aParentWindow, wxWindowID aWindowId,
                                        const wxPoint& aPosition, const wxSize& aSize,
                                        GalType aGalType ) :
    wxWindow( aParentWindow, aWindowId, aPosition, aSize )
{
    m_gal        = NULL;
    m_currentGal = GAL_TYPE_NONE;
    m_view       = NULL;
    m_painter    = NULL;

    SwitchBackend( aGalType, true );
    SetBackgroundStyle( wxBG_STYLE_CUSTOM );

    // Initial display settings
    m_gal->SetLookAtPoint( VECTOR2D( 0, 0 ) );
    m_gal->SetZoomFactor( 1.0 );
    m_gal->ComputeWorldScreenMatrix();

    m_painter = new KiGfx::PCB_PAINTER( m_gal );

    m_view = new KiGfx::VIEW( true );
    m_view->SetPainter( m_painter );
    m_view->SetGAL( m_gal );

    // View uses layers to display EDA_ITEMs (item may be displayed on several layers, for example
    // pad may be shown on pad, pad hole and solder paste layers). There are usual copper layers
    // (eg. F.Cu, B.Cu, internal and so on) and layers for displaying objects such as texts,
    // silkscreen, pads, vias, etc.
    for( int i = 0; i < TOTAL_LAYER_COUNT; i++ )
    {
        m_view->AddLayer( i );
    }

    m_viewControls = new KiGfx::WX_VIEW_CONTROLS( m_view, this );

    Connect( wxEVT_PAINT, wxPaintEventHandler( EDA_DRAW_PANEL_GAL::onPaint ), NULL, this );
    Connect( wxEVT_SIZE, wxSizeEventHandler( EDA_DRAW_PANEL_GAL::onSize ), NULL, this );

    m_timeStamp = 0;
}


EDA_DRAW_PANEL_GAL::~EDA_DRAW_PANEL_GAL()
{
    if( m_painter )
        delete m_painter;

    if( m_viewControls )
        delete m_viewControls;

    if( m_view )
        delete m_view;

    if( m_gal )
        delete m_gal;
}


void EDA_DRAW_PANEL_GAL::onPaint( wxPaintEvent& WXUNUSED( aEvent ) )
{
    Refresh();
}


void EDA_DRAW_PANEL_GAL::onSize( wxSizeEvent& aEvent )
{
    m_gal->ResizeScreen( aEvent.GetSize().x, aEvent.GetSize().y );
}


void EDA_DRAW_PANEL_GAL::Refresh( bool eraseBackground, const wxRect* rect )
{
    const unsigned int FPS_LIMIT = 40;

    // Framerate limiter
    wxLongLong currentTimeStamp = wxGetLocalTimeMillis();
    if( currentTimeStamp - m_timeStamp < ( 1000 / FPS_LIMIT ) )
        return;
    m_timeStamp = currentTimeStamp;

#ifdef __WXDEBUG__
    prof_counter time;

    prof_start( &time, false );
#endif /* __WXDEBUG__ */

    m_gal->BeginDrawing();
    m_gal->SetBackgroundColor( KiGfx::COLOR4D( 0, 0, 0, 1.0 ) );
    m_gal->ClearScreen();

    m_gal->DrawGrid();
    m_view->Redraw();

    m_gal->EndDrawing();

#ifdef __WXDEBUG__
    prof_end( &time );

    wxLogDebug( wxT( "EDA_DRAW_PANEL_GAL::Refresh: %.0f ms (%.0f fps)" ),
        static_cast<double>( time.value ) / 1000.0, 1000000.0 / static_cast<double>( time.value ) );
#endif /* __WXDEBUG__ */
}


void EDA_DRAW_PANEL_GAL::SwitchBackend( GalType aGalType, bool aUseShaders )
{
    wxLogDebug( wxT( "EDA_DRAW_PANEL_GAL::SwitchBackend: using shaders: %s" ),
            aUseShaders ? "true" : "false" );

    // Do not do anything if the currently used GAL is correct
    if( aGalType == m_currentGal && aUseShaders == m_useShaders && m_gal != NULL )
        return;

    delete m_gal;

    switch( aGalType )
    {
    case GAL_TYPE_OPENGL:
        m_gal = new KiGfx::OPENGL_GAL( this, this, this, aUseShaders );
        break;

    case GAL_TYPE_CAIRO:
        m_gal = new KiGfx::CAIRO_GAL( this, this, this );
        break;

    case GAL_TYPE_NONE:
        return;
    }

    m_gal->SetWorldUnitLength( 1.0 / METRIC_UNIT_LENGTH * 2.54 );   // 1 inch in nanometers
    m_gal->SetScreenDPI( 106 );                                     // Display resolution setting
    m_gal->ComputeWorldScreenMatrix();

    if( m_painter )
        m_painter->SetGAL( m_gal );

    if( m_view )
    {
        m_view->SetGAL( m_gal );
        m_view->RecacheAllItems( true );
    }

    wxSize size = GetClientSize();
    m_gal->ResizeScreen( size.GetX(), size.GetY() );

    m_currentGal = aGalType;
    m_useShaders = aUseShaders;
}