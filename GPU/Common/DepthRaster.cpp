#include <algorithm>
#include <cstring>

#include "Common/Math/CrossSIMD.h"
#include "GPU/Common/DepthRaster.h"
#include "GPU/Math3D.h"
#include "Common/Math/math_util.h"
#include "GPU/Common/VertexDecoderCommon.h"

#if PPSSPP_ARCH(SSE2)

struct Vec4S32 {
	__m128i v;

	Vec4S32 operator +(Vec4S32 other) const {
		return Vec4S32{ _mm_add_epi32(v, other.v) };
	}
	Vec4S32 operator -(Vec4S32 other) const {
		return Vec4S32{ _mm_sub_epi32(v, other.v) };
	}
	// This is really bad if we restrict ourselves to SSE2 only.
	// If we have SSE4, we can do _mm_mullo_epi32.
	// Let's avoid using it as much as possible.
	// https://stackoverflow.com/questions/17264399/fastest-way-to-multiply-two-vectors-of-32bit-integers-in-c-with-sse
	Vec4S32 operator *(Vec4S32 other) const {
		__m128i a13 = _mm_shuffle_epi32(v, 0xF5);          // (-,a3,-,a1)
		__m128i b13 = _mm_shuffle_epi32(other.v, 0xF5);          // (-,b3,-,b1)
		__m128i prod02 = _mm_mul_epu32(v, other.v);                 // (-,a2*b2,-,a0*b0)
		__m128i prod13 = _mm_mul_epu32(a13, b13);             // (-,a3*b3,-,a1*b1)
		__m128i prod01 = _mm_unpacklo_epi32(prod02, prod13);   // (-,-,a1*b1,a0*b0) 
		__m128i prod23 = _mm_unpackhi_epi32(prod02, prod13);   // (-,-,a3*b3,a2*b2) 
		return Vec4S32{ _mm_unpacklo_epi64(prod01, prod23) };   // (ab3,ab2,ab1,ab0)
	}
};

struct Vec4F32 {
	__m128 v;

	static Vec4F32 FromVec4S32(Vec4S32 other) {
		return Vec4F32{ _mm_cvtepi32_ps(other.v) };
	}

	Vec4F32 operator +(Vec4F32 other) const {
		return Vec4F32{ _mm_add_ps(v, other.v) };
	}
	Vec4F32 operator -(Vec4F32 other) const {
		return Vec4F32{ _mm_sub_ps(v, other.v) };
	}
	Vec4F32 operator *(Vec4F32 other) const {
		return Vec4F32{ _mm_mul_ps(v, other.v) };
	}
};

#elif PPSSPP_ARCH(ARM_NEON)

struct Vec4S32 {
	uint32x4_t v;

	Vec4S32 operator +(Vec4S32 other) const {
		return Vec4S32{ vaddq_s32(v, other.v) };
	}
	Vec4S32 operator -(Vec4S32 other) const {
		return Vec4S32{ vsubq_s32(v, other.v) };
	}
	Vec4S32 operator *(Vec4S32 other) const {
		return Vec4S32{ vmulq_s32(v, other.v) };
	}
};

struct Vec4F32 {
	float32x4_t v;

	static Vec4F32 FromVec4S32(Vec4S32 other) {
		return Vec4F32{ _mm_cvtepi32_ps(other.v) };
	}

	Vec4F32 operator +(Vec4F32 other) const {
		return Vec4F32{ vaddq_f32(v, other.v) };
	}
	Vec4F32 operator -(Vec4F32 other) const {
		return Vec4F32{ vsubq_f32(v, other.v) };
	}
	Vec4F32 operator *(Vec4F32 other) const {
		return Vec4F32{ vmulq_f32(v, other.v) };
	}
};

#else

struct Vec4S32 {
	s32 v[4];
};

#endif

struct ScreenVert {
	int x;
	int y;
	uint16_t z;
	uint16_t behind;
};

void DepthRasterRect(uint16_t *dest, int stride, int x1, int y1, int x2, int y2, short depthValue, GEComparison depthCompare) {
	// Swap coordinates if needed, we don't back-face-cull rects.
	// We also ignore the UV rotation here.
	if (x1 > x2) {
		std::swap(x1, x2);
	}
	if (y1 > y2) {
		std::swap(y1, y2);
	}
	if (x1 == x2 || y1 == y2) {
		return;
	}

#if PPSSPP_ARCH(SSE2)
	__m128i valueX8 = _mm_set1_epi16(depthValue);
	for (int y = y1; y < y2; y++) {
		__m128i *ptr = (__m128i *)(dest + stride * y + x1);
		int w = x2 - x1;
		switch (depthCompare) {
		case GE_COMP_ALWAYS:
			if (depthValue == 0) {
				memset(ptr, 0, w * 2);
			} else {
				while (w >= 8) {
					_mm_storeu_si128(ptr, valueX8);
					ptr++;
					w -= 8;
				}
			}
			break;
			// TODO: Trailer
		case GE_COMP_NEVER:
			break;
		default:
			// TODO
			break;
		}
	}

#elif PPSSPP_ARCH(ARM64_NEON)
	uint16x8_t valueX8 = vdupq_n_u16(depthValue);
	for (int y = y1; y < y2; y++) {
		uint16_t *ptr = (uint16_t *)(dest + stride * y + x1);
		int w = x2 - x1;

		switch (depthCompare) {
		case GE_COMP_ALWAYS:
			if (depthValue == 0) {
				memset(ptr, 0, w * 2);
			} else {
				while (w >= 8) {
					vst1q_u16(ptr, valueX8);
					ptr += 8;
					w -= 8;
				}
			}
			break;
			// TODO: Trailer
		case GE_COMP_NEVER:
			break;
		default:
			// TODO
			break;
		}
	}
#else
	// Do nothing for now
#endif
}

using namespace Math3D;
struct int2 {
	int x, y;
	int2(float a, float b) {
		x = (int)(a + 0.5f);
		y = (int)(b + 0.5f);
	}
};

// Adapted from Intel's depth rasterizer example.
// Started with the scalar version, will SIMD-ify later.
// x1/y1 etc are the scissor rect.
void DepthRasterTriangle(uint16_t *depthBuf, int stride, int x1, int y1, int x2, int y2, const ScreenVert vertsSub[3], GEComparison compareMode) {
	int tileStartX = x1;
	int tileEndX = x2;

	int tileStartY = y1;
	int tileEndY = y2;

	// BEGIN triangle setup. This should be done SIMD, four triangles at a time.
	// Due to the many multiplications, we might want to do it in floating point as 32-bit integer muls
	// are slow on SSE2.

	// Convert to whole pixels for now. Later subpixel precision.
	ScreenVert verts[3];
	verts[0].x = vertsSub[0].x;
	verts[0].y = vertsSub[0].y;
	verts[0].z = vertsSub[0].z;
	verts[1].x = vertsSub[2].x;
	verts[1].y = vertsSub[2].y;
	verts[1].z = vertsSub[2].z;
	verts[2].x = vertsSub[1].x;
	verts[2].y = vertsSub[1].y;
	verts[2].z = vertsSub[1].z;

	// use fixed-point only for X and Y.  Avoid work for Z and W.
	int startX = std::max(std::min(std::min(verts[0].x, verts[1].x), verts[2].x), tileStartX);
	int endX = std::min(std::max(std::max(verts[0].x, verts[1].x), verts[2].x) + 1, tileEndX);

	int startY = std::max(std::min(std::min(verts[0].y, verts[1].y), verts[2].y), tileStartY);
	int endY = std::min(std::max(std::max(verts[0].y, verts[1].y), verts[2].y) + 1, tileEndY);
	if (endX == startX || endY == startY) {
		// No pixels
		return;
	}
	// TODO: Cull really small triangles here.

	// Fab(x, y) =     Ax       +       By     +      C              = 0
	// Fab(x, y) = (ya - yb)x   +   (xb - xa)y + (xa * yb - xb * ya) = 0
	// Compute A = (ya - yb) for the 3 line segments that make up each triangle
	int A0 = verts[1].y - verts[2].y;
	int A1 = verts[2].y - verts[0].y;
	int A2 = verts[0].y - verts[1].y;

	// Compute B = (xb - xa) for the 3 line segments that make up each triangle
	int B0 = verts[2].x - verts[1].x;
	int B1 = verts[0].x - verts[2].x;
	int B2 = verts[1].x - verts[0].x;

	// Compute C = (xa * yb - xb * ya) for the 3 line segments that make up each triangle
	int C0 = verts[1].x * verts[2].y - verts[2].x * verts[1].y;
	int C1 = verts[2].x * verts[0].y - verts[0].x * verts[2].y;
	int C2 = verts[0].x * verts[1].y - verts[1].x * verts[0].y;

	// Compute triangle area
	int triArea = A0 * verts[0].x + B0 * verts[0].y + C0;
	if (triArea <= 0) {
		// Too small to rasterize or backface culled
		// NOTE: Just disabling this check won't enable two-sided rendering.
		// Since it's not that common, let's just queue the triangles with both windings.
		return;
	}

	int rowIdx = (startY * stride + startX);
	int col = startX;
	int row = startY;

	// Calculate slopes at starting corner.
	int alpha0 = (A0 * col) + (B0 * row) + C0;
	int beta0 = (A1 * col) + (B1 * row) + C1;
	int gamma0 = (A2 * col) + (B2 * row) + C2;

	float oneOverTriArea = (1.0f / float(triArea));

	// END triangle setup.
	float zz[3];
	for (int vv = 0; vv < 3; vv++) {
		zz[vv] = (float)verts[vv].z * oneOverTriArea;
	}

	// Incrementally compute Fab(x, y) for all the pixels inside the bounding box formed by (startX, endX) and (startY, endY)
	for (int r = startY; r < endY; r++,
		row++,
		rowIdx += stride,
		alpha0 += B0,
		beta0 += B1,
		gamma0 += B2)
	{
		int idx = rowIdx;

		// Restore row steppers.
		int alpha = alpha0;
		int beta = beta0;
		int gamma = gamma0;

		for (int c = startX; c < endX; c++,
			idx++,
			alpha += A0,
			beta += A1,
			gamma += A2)
		{
			int mask = alpha >= 0 && beta >= 0 && gamma >= 0;
			// Early out if all of this quad's pixels are outside the triangle.
			if (!mask) {
				continue;
			}
			// Compute barycentric-interpolated depth
			float depth = alpha * zz[0] + beta * zz[1] + gamma * zz[2];
			float previousDepthValue = (float)depthBuf[idx];

			int depthMask;
			switch (compareMode) {
			case GE_COMP_EQUAL:  depthMask = depth == previousDepthValue; break;
			case GE_COMP_LESS: depthMask = depth < previousDepthValue; break;
			case GE_COMP_LEQUAL: depthMask = depth <= previousDepthValue; break;
			case GE_COMP_GEQUAL: depthMask = depth >= previousDepthValue; break;
			case GE_COMP_GREATER: depthMask = depth > previousDepthValue; break;
			case GE_COMP_NOTEQUAL: depthMask = depth != previousDepthValue; break;
			case GE_COMP_ALWAYS:
			default:
				depthMask = 1;
				break;
			}
			int finalMask = mask & depthMask;
			depth = finalMask == 1 ? depth : previousDepthValue;
			depthBuf[idx] = (u16)depth;
		} //for each column
	} // for each row
}

// We ignore lots of primitive types for now.
void DepthRasterPrim(uint16_t *depth, int depthStride, int x1, int y1, int x2, int y2, void *bufferData,
	const void *vertexData, const void *indexData, GEPrimitiveType prim, int count, VertexDecoder *dec, u32 vertTypeID, bool clockwise) {

	GEComparison compareMode = gstate.getDepthTestFunction();
	if (gstate.isModeClear()) {
		if (!gstate.isClearModeDepthMask()) {
			return;
		}
		compareMode = GE_COMP_ALWAYS;
	} else {
		if (!gstate.isDepthTestEnabled() || !gstate.isDepthWriteEnabled())
			return;
	}

	switch (prim) {
	case GE_PRIM_INVALID:
	case GE_PRIM_KEEP_PREVIOUS:
	case GE_PRIM_LINES:
	case GE_PRIM_LINE_STRIP:
	case GE_PRIM_POINTS:
		return;
	default:
		break;
	}

	// TODO: Ditch indexed primitives for now, also ditched skinned ones since we don't have a fast way to skin without
	// running the full decoder.
	if (vertTypeID & (GE_VTYPE_IDX_MASK | GE_VTYPE_WEIGHT_MASK)) {
		return;
	}

	bool isThroughMode = (vertTypeID & GE_VTYPE_THROUGH_MASK) != 0;
	bool cullEnabled = false;
	bool cullCCW = false;

	// Turn the input data into a raw float array that we can pass to an optimized triangle rasterizer.
	float *verts = (float *)bufferData;
	ScreenVert *screenVerts = (ScreenVert *)((uint8_t *)bufferData + 65536 * 8);

	// Simple, most common case.
	int vertexStride = dec->VertexSize();
	int offset = dec->posoff;
	float factor = 1.0f;
	switch (vertTypeID & GE_VTYPE_POS_MASK) {
	case GE_VTYPE_POS_8BIT:
		if (!isThroughMode) {
			factor = 1.0f / 128.0f;
		}
		for (int i = 0; i < count; i++) {
			const s8 *data = (const s8 *)vertexData + i * vertexStride + offset;
			for (int j = 0; j < 3; j++) {
				verts[i * 3 + j] = data[j] * factor;
			}
		}
		break; 
	case GE_VTYPE_POS_16BIT:
		if (!isThroughMode) {
			factor = 1.0f / 32768.0f;
		}
		for (int i = 0; i < count; i++) {
			const s16 *data = ((const s16 *)((const s8 *)vertexData + i * vertexStride + offset));
			for (int j = 0; j < 3; j++) {
				verts[i * 3 + j] = data[j] * factor;
			}
		}
		break;
	case GE_VTYPE_POS_FLOAT:
		for (int i = 0; i < count; i++)
			memcpy(&verts[i * 3], (const u8 *)vertexData + vertexStride * i + offset, sizeof(float) * 3);
		break;
	}

	// OK, we now have the coordinates. Let's transform, we can actually do this in-place.
	if (!(vertTypeID & GE_VTYPE_THROUGH_MASK)) {
		cullEnabled = gstate.isCullEnabled();

		// TODO: This is very suboptimal. This should be one matrix multiplication per vertex.

		float viewportX = gstate.getViewportXCenter();
		float viewportY = gstate.getViewportYCenter();
		float viewportZ = gstate.getViewportZCenter();
		float viewportScaleX = gstate.getViewportXScale();
		float viewportScaleY = gstate.getViewportYScale();
		float viewportScaleZ = gstate.getViewportZScale();
		
		bool allBehind = true;

		for (int i = 0; i < count; i++) {
			float world[3];
			float view[3];
			float proj[4];
			Vec3ByMatrix43(world, verts + i * 3, gstate.worldMatrix);
			Vec3ByMatrix43(view, world, gstate.viewMatrix);
			Vec3ByMatrix44(proj, view, gstate.projMatrix);  // TODO: Include adjustments to the proj matrix?

			float w = proj[3];

			bool inFront = w > 0.0f;
			screenVerts[i].behind = !inFront;
			if (inFront) {
				allBehind = false;
			}

			// Clip to the w=0 plane.
			proj[0] /= w;
			proj[1] /= w;
			proj[2] /= w;

			// Then transform by the viewport and offset to finally get subpixel coordinates. Normally, this is done by the viewport
			// and offset params.
			float screen[3];
			screen[0] = (proj[0] * viewportScaleX + viewportX) * 16.0f - gstate.getOffsetX16();
			screen[1] = (proj[1] * viewportScaleY + viewportY) * 16.0f - gstate.getOffsetY16();
			screen[2] = (proj[2] * viewportScaleZ + viewportZ);
			if (screen[2] < 0.0f) {
				screen[2] = 0.0f;
			}
			if (screen[2] >= 65535.0f) {
				screen[2] = 65535.0f;
			}
			screenVerts[i].x = screen[0] * (1.0f / 16.0f);  // We ditch the subpixel precision here.
			screenVerts[i].y = screen[1] * (1.0f / 16.0f);
			screenVerts[i].z = screen[2];
		}
		if (allBehind) {
			// Cull the whole draw.
			return;
		}
	} else {
		for (int i = 0; i < count; i++) {
			screenVerts[i].x = (int)verts[i * 3 + 0];
			screenVerts[i].y = (int)verts[i * 3 + 1];
			screenVerts[i].z = (u16)clamp_value(verts[i * 3 + 2], 0.0f, 65535.0f);
		}
	}

	// Then we need to stitch primitives from strips, etc etc...
	// For now we'll just do it tri by tri. Later let's be more efficient.
	
	switch (prim) {
	case GE_PRIM_RECTANGLES:
		for (int i = 0; i < count / 2; i++) {
			uint16_t z = screenVerts[i + 1].z;  // depth from second vertex
			// TODO: Should clip coordinates to the scissor rectangle.
			// We remove the subpixel information here.
			DepthRasterRect(depth, depthStride, screenVerts[i].x, screenVerts[i].y, screenVerts[i + 1].x, screenVerts[i + 1].y,
				z, compareMode);
		}
		break;
	case GE_PRIM_TRIANGLES:
		for (int i = 0; i < count / 3; i++) {
			if (screenVerts[i * 3].behind || screenVerts[i * 3 + 1].behind || screenVerts[i * 3 + 2].behind) {
				continue;
			}
			DepthRasterTriangle(depth, depthStride, x1, y1, x2, y2, screenVerts + i * 3, compareMode);
		}
		break;
	case GE_PRIM_TRIANGLE_STRIP:
	{
		int wind = 2;
		for (int i = 0; i < count - 2; i++) {
			int i0 = i;
			int i1 = i + wind;
			wind ^= 3;
			int i2 = i + wind;
			if (screenVerts[i0].behind || screenVerts[i1].behind || screenVerts[i2].behind) {
				continue;
			}
			ScreenVert v[3];
			v[0] = screenVerts[i0];
			v[1] = screenVerts[i1];
			v[2] = screenVerts[i2];
			DepthRasterTriangle(depth, depthStride, x1, y1, x2, y2, v, compareMode);
		}
		break;
	}
	}
}
