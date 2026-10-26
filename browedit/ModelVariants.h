#pragma once

class BrowEdit;

// makes recoloured copies of a model: new textures + a copy of the .rsm/.rsm2 (same version) that uses them
namespace modelvariants
{
	extern bool visible;
	void showWindow(BrowEdit* browEdit);
}
