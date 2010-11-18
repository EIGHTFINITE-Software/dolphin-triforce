// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

// ---------------------------------------------------------------------------------------------
// GC graphics pipeline
// ---------------------------------------------------------------------------------------------
// 3d commands are issued through the fifo. The gpu draws to the 2MB EFB.
// The efb can be copied back into ram in two forms: as textures or as XFB.
// The XFB is the region in RAM that the VI chip scans out to the television.
// So, after all rendering to EFB is done, the image is copied into one of two XFBs in RAM.
// Next frame, that one is scanned out and the other one gets the copy. = double buffering.
// ---------------------------------------------------------------------------------------------


#include "RenderBase.h"
#include "Atomic.h"
#include "MainBase.h"
#include "VideoConfig.h"
#include "FramebufferManagerBase.h"
#include "Fifo.h"
#include "Timer.h"
#include "StringUtil.h"

#include <cmath>
#include <string>

// TODO: Move these out of here.
int frameCount;
//int OSDChoice, OSDTime, OSDInternalW, OSDInternalH;

SVideoInitialize g_VideoInitialize;
PLUGIN_GLOBALS* globals;

Renderer *g_renderer;

bool s_bLastFrameDumped = false;
Common::CriticalSection Renderer::s_criticalScreenshot;
std::string Renderer::s_sScreenshotName;

volatile bool Renderer::s_bScreenshot;

// The framebuffer size
int Renderer::s_target_width;
int Renderer::s_target_height;

// The custom resolution
int Renderer::s_Fulltarget_width;
int Renderer::s_Fulltarget_height;

// TODO: Add functionality to reinit all the render targets when the window is resized.
int Renderer::s_backbuffer_width;
int Renderer::s_backbuffer_height;

// Internal resolution scale (related to xScale/yScale for "Auto" scaling)
float Renderer::EFBxScale;
float Renderer::EFByScale;

// ratio of backbuffer size and render area size
float Renderer::xScale;
float Renderer::yScale;

unsigned int Renderer::s_XFB_width;
unsigned int Renderer::s_XFB_height;

int Renderer::s_LastEFBScale;

bool Renderer::s_skipSwap;
bool Renderer::XFBWrited;

Renderer::Renderer()
{
	UpdateActiveConfig();
}

Renderer::~Renderer()
{

}

void Renderer::RenderToXFB(u32 xfbAddr, u32 fbWidth, u32 fbHeight, const EFBRectangle& sourceRc)
{
	if (!fbWidth || !fbHeight)
		return;

	s_skipSwap = g_bSkipCurrentFrame;

	VideoFifo_CheckEFBAccess();
	VideoFifo_CheckSwapRequestAt(xfbAddr, fbWidth, fbHeight);
	XFBWrited = true;

	// XXX: Without the VI, how would we know what kind of field this is? So
	// just use progressive.
	if (g_ActiveConfig.bUseXFB)
	{
		FramebufferManagerBase::CopyToXFB(xfbAddr, fbWidth, fbHeight, sourceRc);
	}
	else
	{
		g_renderer->Swap(xfbAddr, FIELD_PROGRESSIVE, fbWidth, fbHeight,sourceRc);
		Common::AtomicStoreRelease(s_swapRequested, FALSE);
	}
}

// return true if target size changed
bool Renderer::CalculateTargetSize(float multiplier)
{
	switch (s_LastEFBScale)
	{
		case 0:
			EFBxScale = xScale;
			EFByScale = yScale;
			break;
		case 1:
			EFBxScale = ceilf(xScale);
			EFByScale = ceilf(yScale);
			break;
		default:
			EFBxScale = EFByScale = (float)(g_ActiveConfig.iEFBScale - 1);
			break;
	};

	EFBxScale *= multiplier;
	EFByScale *= multiplier;

	const int m_newFrameBufferWidth  = (int)(EFB_WIDTH * EFBxScale);
	const int m_newFrameBufferHeight = (int)(EFB_HEIGHT * EFByScale);

	if (m_newFrameBufferWidth != s_target_width ||
		m_newFrameBufferHeight != s_target_height)
	{
		s_Fulltarget_width  = s_target_width  = m_newFrameBufferWidth;
		s_Fulltarget_height = s_target_height = m_newFrameBufferHeight;
		
		return true;
	}
	
	return false;
}

void Renderer::SetScreenshot(const char *filename)
{
        s_criticalScreenshot.Enter();
        s_sScreenshotName = filename;
        s_bScreenshot = true;
        s_criticalScreenshot.Leave();
}

// Create On-Screen-Messages
void Renderer::DrawDebugText()
{
	// OSD Menu messages
	if (g_ActiveConfig.bOSDHotKey)
	{
		if (OSDChoice > 0)
		{
			OSDTime = Common::Timer::GetTimeMs() + 3000;
			OSDChoice = -OSDChoice;
		}
		if ((u32)OSDTime > Common::Timer::GetTimeMs())
		{
			const char* res_text = "";
			switch (g_ActiveConfig.iEFBScale)
			{
			case 0:
				res_text = "Auto (fractional)";
				break;
			case 1:
				res_text = "Auto (integral)";
				break;
			case 2:
				res_text = "Native";
				break;
			case 3:
				res_text = "2x";
				break;
			case 4:
				res_text = "3x";
				break;
			}

			const char* ar_text = "";
			switch(g_ActiveConfig.iAspectRatio)
			{
			case ASPECT_AUTO:
				ar_text = "Auto";
				break;
			case ASPECT_FORCE_16_9:
				ar_text = "16:9";
				break;
			case ASPECT_FORCE_4_3:
				ar_text = "4:3";
				break;
			case ASPECT_STRETCH:
				ar_text = "Stretch";
				break;
			}

			const char* const efbcopy_text = g_ActiveConfig.bEFBCopyDisable ? "Disabled" :
				g_ActiveConfig.bCopyEFBToTexture ? "to Texture" : "to RAM";

			// The rows
			const std::string lines[] =
			{
				std::string("3: Internal Resolution: ") + res_text,
				std::string("4: Aspect Ratio: ") + ar_text + (g_ActiveConfig.bCrop ? " (crop)" : ""),
				std::string("5: Copy EFB: ") + efbcopy_text,
				std::string("6: Fog: ") + (g_ActiveConfig.bDisableFog ? "Disabled" : "Enabled"),
				std::string("7: Material Lighting: ") + (g_ActiveConfig.bDisableLighting ? "Disabled" : "Enabled"),
			};

			enum { lines_count = sizeof(lines)/sizeof(*lines) };

			std::string final_yellow, final_cyan;

			// If there is more text than this we will have a collision
			if (g_ActiveConfig.bShowFPS)
			{
				final_yellow = final_cyan = "\n\n";
			}

			// The latest changed setting in yellow
			for (int i = 0; i != lines_count; ++i)
			{
				if (OSDChoice == -i - 1)
					final_yellow += lines[i];
				final_yellow += '\n';
			}

			// The other settings in cyan
			for (int i = 0; i != lines_count; ++i)
			{
				if (OSDChoice != -i - 1)
					final_cyan += lines[i];
				final_cyan += '\n';
			}

			// Render a shadow
			g_renderer->RenderText(final_cyan.c_str(), 21, 21, 0xDD000000);
			g_renderer->RenderText(final_yellow.c_str(), 21, 21, 0xDD000000);
			//and then the text
			g_renderer->RenderText(final_cyan.c_str(), 20, 20, 0xFF00FFFF);
			g_renderer->RenderText(final_yellow.c_str(), 20, 20, 0xFFFFFF00);
		}
	}
}

void UpdateViewport()
{
	g_renderer->UpdateViewport();
}