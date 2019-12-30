/* DarkMark (C) 2019 Stephane Charette <stephanecharette@gmail.com>
 * $Id$
 */

#include "DarkMark.hpp"


dm::SettingsWnd::SettingsWnd(dm::DMContent & c) :
	DocumentWindow("DarkMark v" DARKMARK_VERSION " - Settings", Colours::darkgrey, TitleBarButtons::closeButton),
	content(c),
	ok_button("OK")
{
	setContentNonOwned		(&canvas, true	);
	setUsingNativeTitleBar	(true			);
	setResizable			(true, false	);
	setDropShadowEnabled	(true			);
	setAlwaysOnTop			(true			);

	canvas.addAndMakeVisible(pp);
	canvas.addAndMakeVisible(ok_button);

	ok_button.addListener(this);

	setIcon(DarkMarkLogo());
	ComponentPeer *peer = getPeer();
	if (peer)
	{
		peer->setIcon(DarkMarkLogo());
	}

	v_darkhelp_threshold							= std::round(100.0f * dmapp().darkhelp->threshold);
	v_darkhelp_hierchy_threshold					= std::round(100.0f * dmapp().darkhelp->hierarchy_threshold);
	v_darkhelp_non_maximal_suppression_threshold	= std::round(100.0f * dmapp().darkhelp->non_maximal_suppression_threshold);

	v_darkhelp_threshold						.addListener(this);
	v_darkhelp_hierchy_threshold				.addListener(this);
	v_darkhelp_non_maximal_suppression_threshold.addListener(this);

	Array<PropertyComponent*> properties;
//	TextPropertyComponent		* t = nullptr;
//	BooleanPropertyComponent	* b = nullptr;
	SliderPropertyComponent		* s = nullptr;

	s = new SliderPropertyComponent(v_darkhelp_threshold, "detection threshold", 0, 100, 1);
	s->setTooltip("Detection threshold is used to determine whether or not there is an object in the predicted bounding box.");
	properties.add(s);

	s = new SliderPropertyComponent(v_darkhelp_hierchy_threshold, "hierarchy threshold", 0, 100, 1);
	s->setTooltip("The hierarchical threshold is used to decide whether following the tree to a more specific class is the right action to take. When this threshold is 0, the tree will basically follow the highest probability branch all the way to a leaf node.");
	properties.add(s);

	s = new SliderPropertyComponent(v_darkhelp_non_maximal_suppression_threshold, "nms threshold", 0, 100, 1);
	s->setTooltip("Non-Maximal Suppression (NMS) suppresses overlapping bounding boxes and only retains the bounding box that has the maximum probability of object detection associated with it. It examines all bounding boxes and removes the least confident of the boxes that overlap with each other.");
	properties.add(s);

	pp.addSection("darknet", properties);
	properties.clear();

	auto r = dmapp().wnd->getBounds();
	r = r.withSizeKeepingCentre(400, 200);
	setBounds(r);

	setVisible(true);

	return;
}


dm::SettingsWnd::~SettingsWnd()
{
	return;
}


void dm::SettingsWnd::closeButtonPressed()
{
	// close button

	dmapp().settings_wnd.reset(nullptr);

	return;
}


void dm::SettingsWnd::userTriedToCloseWindow()
{
	// ALT+F4

	dmapp().settings_wnd.reset(nullptr);

	return;
}


void dm::SettingsWnd::resized()
{
	// get the document window to resize the canvas, then we'll deal with the rest of the components
	DocumentWindow::resized();

	const int margin_size = 5;

	FlexBox button_row;
	button_row.flexDirection = FlexBox::Direction::row;
	button_row.justifyContent = FlexBox::JustifyContent::flexEnd;
	button_row.items.add(FlexItem(ok_button).withWidth(100.0));

	FlexBox fb;
	fb.flexDirection = FlexBox::Direction::column;
	fb.items.add(FlexItem(pp).withFlex(1.0));
	fb.items.add(FlexItem(button_row).withHeight(30.0));

	auto r = getLocalBounds();
	r.reduce(margin_size, margin_size);
	fb.performLayout(r);

	return;
}


void dm::SettingsWnd::buttonClicked(Button * button)
{
	closeButtonPressed();
	return;
}


void dm::SettingsWnd::valueChanged(Value & value)
{
	dmapp().darkhelp->hierarchy_threshold				= static_cast<float>(v_darkhelp_hierchy_threshold					.getValue()) / 100.0f;
	dmapp().darkhelp->non_maximal_suppression_threshold	= static_cast<float>(v_darkhelp_non_maximal_suppression_threshold	.getValue()) / 100.0f;
	dmapp().darkhelp->threshold							= static_cast<float>(v_darkhelp_threshold							.getValue()) / 100.0f;

	startTimer(250); // request a callback -- in milliseconds -- at which point in time we'll fully reload the current image

	return;
}


void dm::SettingsWnd::timerCallback()
{
	// if we get called, then the settings are no longer changing, so reload the current image

	stopTimer();

	content.load_image(content.image_filename_index);

	return;
}