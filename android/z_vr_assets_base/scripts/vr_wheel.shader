// selector-wheel hologram for a weapon with no ammo; the tint comes from the entity colour
gfx/misc/wheel_empty
{
	cull disable
	{
		map $whiteimage
		blendFunc GL_ONE GL_ONE
		rgbGen entity
	}
	{
		map gfx/misc/red_scanlines.jpg
		blendFunc GL_ONE GL_ONE
		rgbGen entity
		tcGen environment
		tcMod scroll 0 0.4
	}
}
