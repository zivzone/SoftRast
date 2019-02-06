#include "Renderer.h"
#include "Rasterizer_new.h"
#include "kt/Memory.h"
#include "kt/Logging.h"

namespace sr
{


FrameBuffer::FrameBuffer(uint32_t _width, uint32_t _height, bool _colour /*= true*/, bool _depth /*= true*/)
{
	Init(_width, _height, _colour, _depth);
}

FrameBuffer::~FrameBuffer()
{
	kt::Free(m_depthTiles);
	kt::Free(m_colourTiles);
}

void FrameBuffer::Init(uint32_t _width, uint32_t _height, bool _colour /*= true*/, bool _depth /*= true*/)
{
	m_height = _height;
	m_width = _width;

	m_tilesX = (uint32_t)kt::AlignValue(m_width, Config::c_binWidth) >> Config::c_binWidthLog2;
	m_tilesY = (uint32_t)kt::AlignValue(m_height, Config::c_binHeight) >> Config::c_binHeightLog2;

	if (_colour)
	{
		m_colourTiles = (ColourTile*)kt::Malloc(sizeof(ColourTile) * m_tilesX * m_tilesY, KT_ALIGNOF(ColourTile));
	}

	if (_depth)
	{
		m_depthTiles = (DepthTile*)kt::Malloc(sizeof(DepthTile) * m_tilesX * m_tilesY, KT_ALIGNOF(DepthTile));
	}
}

void FrameBuffer::Blit(uint8_t* _linearFramebuffer)
{
	// hack/slow
	uint32_t* fb32 = (uint32_t*)_linearFramebuffer;
	for (uint32_t tileY = 0; tileY < m_tilesY; ++tileY)
	{
		for (uint32_t tileX = 0; tileX < m_tilesX; ++tileX)
		{
			ColourTile const& tile = m_colourTiles[tileY * m_tilesX + tileX];

			uint32_t const yEnd = kt::Min(Config::c_binHeight, m_height - tileY * Config::c_binHeight);
			uint32_t const widthCopySize = kt::Min(Config::c_binWidth, m_width - tileX * Config::c_binWidth);

			for (uint32_t y = 0; y < yEnd; ++y)
			{
				uint8_t const* src = &tile.m_colour[y * 4 * Config::c_binWidth];

				uint32_t* dest = fb32 + tileY * Config::c_binHeight * m_width + tileX * Config::c_binWidth + y * m_width;
				memcpy(dest, src, 4 * widthCopySize);
			}
		}
	}
}

void FrameBuffer::BlitDepth(uint8_t* _linearFramebuffer)
{
	// hack/slow
	uint32_t* fb32 = (uint32_t*)_linearFramebuffer;
	for (uint32_t tileY = 0; tileY < m_tilesY; ++tileY)
	{
		for (uint32_t tileX = 0; tileX < m_tilesX; ++tileX)
		{
			DepthTile const& tile = m_depthTiles[tileY * m_tilesX + tileX];

			uint32_t const yEnd = kt::Min(Config::c_binHeight, m_height - tileY * Config::c_binHeight);
			uint32_t const widthCopySize = kt::Min(Config::c_binWidth, m_width - tileX * Config::c_binWidth);

			for (uint32_t y = 0; y < yEnd; ++y)
			{
				float const* src = &tile.m_depth[y * Config::c_binWidth];
				uint32_t* dest = fb32 + tileY * Config::c_binHeight * m_width + tileX * Config::c_binWidth + y * m_width;
				for (uint32_t x = 0; x < widthCopySize; ++x)
				{
					*dest++ = uint8_t(*src++ * 255.0f);
				}
			}
		}
	}
}

DrawCall::DrawCall()
	: m_colourWrite(1)
	, m_depthRead(1)
	, m_depthWrite(1)
{
}

DrawCall& DrawCall::SetPixelShader(PixelShaderFn _fn, void const* _uniform)
{
	m_pixelShader = _fn;
	m_pixelUniforms = _uniform;
	return *this;
}

DrawCall& DrawCall::SetIndexBuffer(void const* _buffer, uint32_t const _stride, uint32_t const _num)
{
	m_indexBuffer.m_num = _num;
	m_indexBuffer.m_ptr = _buffer;
	m_indexBuffer.m_stride = _stride;
	return *this;
}

DrawCall& DrawCall::SetPositionBuffer(void const* _buffer, uint32_t const _stride, uint32_t const _num)
{
	m_positionBuffer.m_num = _num;
	m_positionBuffer.m_ptr = _buffer;
	m_positionBuffer.m_stride = _stride;
	return *this;
}

DrawCall& DrawCall::SetAttributeBuffer(void const* _buffer, uint32_t const _stride, uint32_t const _num)
{
	m_attributeBuffer.m_num = _num;
	m_attributeBuffer.m_ptr = _buffer;
	m_attributeBuffer.m_stride = _stride;
	return *this;
}

DrawCall& DrawCall::SetFrameBuffer(FrameBuffer const* _buffer)
{
	m_frameBuffer = _buffer;
	return *this;
}


DrawCall& DrawCall::SetMVP(kt::Mat4 const& _mvp)
{
	m_mvp = _mvp;
	return *this;
}

RenderContext::RenderContext()
{
	// todo: hard coded
	m_allocator.Init(kt::GetDefaultAllocator(), 1024 * 1024 * 512);
	m_binner.Init(1, uint32_t(kt::AlignValue(1280, Config::c_binWidth)) / Config::c_binWidth, uint32_t(kt::AlignValue(720, Config::c_binHeight)) / Config::c_binHeight);
}

RenderContext::~RenderContext()
{
	m_allocator.Reset();
}

void RenderContext::DrawIndexed(DrawCall const& _call)
{
	KT_ASSERT(_call.m_indexBuffer.m_ptr && "No index buffer bound.");
	m_drawCalls.PushBack(_call);
	m_drawCalls.Back().m_drawCallIdx = m_drawCalls.Size() - 1;
}

void RenderContext::ClearFrameBuffer(FrameBuffer& _buffer, uint32_t _color, bool _clearColour /*= true*/, bool _clearDepth /*= true*/)
{
	if (_clearDepth)
	{
		for (uint32_t i = 0; i < (_buffer.m_tilesX * _buffer.m_tilesY); ++i)
		{
			for (uint32_t j = 0; j < (Config::c_binHeight * Config::c_binWidth); ++j)
			{
				_buffer.m_depthTiles[i].m_depth[j] = 1.0f;
			}
		}
	}

	if (_clearColour)
	{
		for (uint32_t i = 0; i < (_buffer.m_tilesX * _buffer.m_tilesY); ++i)
		{	
			memset(_buffer.m_colourTiles[i].m_colour, _color, sizeof(_buffer.m_colourTiles[i].m_colour));
		}
	}
}

void RenderContext::BeginFrame()
{
	m_drawCalls.Clear();
	m_allocator.Reset();
}

void RenderContext::EndFrame()
{
	for (uint32_t i = 0; i < m_binner.m_numBinsX * m_binner.m_numBinsY * m_binner.m_numThreads; ++i)
	{
		// todo frame number dirty
		m_binner.m_bins[i].m_numChunks = 0;
	}

	for (uint32_t i = 0; i < m_drawCalls.Size(); ++i)
	{
		DrawCall const& draw = m_drawCalls[i];
		BinTrisEntry(m_binner, m_allocator, 0, 0, draw.m_indexBuffer.m_num / 3, draw);
	}

	uint32_t activeBins = 0;
	uint32_t activeChunks = 0;


	for (uint32_t binY = 0; binY < m_binner.m_numBinsY; ++binY)
	{
		for (uint32_t binX = 0; binX < m_binner.m_numBinsX; ++binX)
		{
			for (uint32_t threadIdx = 0; threadIdx < m_binner.m_numThreads; ++threadIdx)
			{
				ThreadBin& bin = m_binner.LookupThreadBin(threadIdx, binX, binY);
				activeBins += bin.m_numChunks != 0;
				for (uint32_t j = 0; j < bin.m_numChunks; ++j)
				{
					activeChunks++;

					DrawCall const& call = m_drawCalls[bin.m_drawCallIndicies[j]];
					uint32_t tileIdx = binY * m_binner.m_numBinsX + binX;
					RasterTrisInBin(call, *bin.m_binChunks[j], &call.m_frameBuffer->m_depthTiles[tileIdx], &call.m_frameBuffer->m_colourTiles[tileIdx]);
				}
			}
		}
	}

	KT_LOG_INFO("%u bins, %u chunks", activeBins, activeChunks);
}


}