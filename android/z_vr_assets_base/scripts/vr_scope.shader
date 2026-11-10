// gfx/misc/zoom_mask2 without its scanline stage: that stage's 2x blend over-brightens in VR, and stacks with the vignette copies
gfx/misc/zoom_mask_vr
{
	nomipmaps
	nopicmip
	cull disable
	{
		map gfx/misc/zoom_mask2.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen exactVertex
		alphaGen vertex
	}
}
