/* DarkMark (C) 2019-2020 Stephane Charette <stephanecharette@gmail.com>
 * $Id$
 */

#include "DarkMark.hpp"

#include "json.hpp"
using json = nlohmann::json;


dm::DMContent::DMContent(const std::string & prefix) :
	cfg_prefix(prefix),
	canvas(*this),
	scrollfield(*this),
	scrollfield_width(cfg().get_int("scrollfield_width")),
	empty_image_name_index(0),
	sort_order(static_cast<ESort>(cfg().get_int("sort_order"))),
	show_labels(static_cast<EToggle>(cfg().get_int("show_labels"))),
	show_predictions(static_cast<EToggle>(cfg().get_int("show_predictions"))),
	image_is_completely_empty(false),
	show_marks(cfg().get_bool("show_marks")),
	marks_are_shown(false),
	predictions_are_shown(false),
	number_of_marks(0),
	number_of_predictions(0),
	alpha_blend_percentage(static_cast<double>(cfg().get_int("alpha_blend_percentage")) / 100.0),
	shade_rectangles(cfg().get_bool("shade_rectangles")),
	all_marks_are_bold(cfg().get_bool("all_marks_are_bold")),
	show_processing_time(cfg().get_bool("show_processing_time")),
	need_to_save(false),
	selected_mark(-1),
	scale_factor(1.0),
	most_recent_class_idx(0),
	image_filename_index(0),
	project_info(cfg_prefix)
{
	addAndMakeVisible(canvas);
	addAndMakeVisible(scrollfield);

	addAndMakeVisible(bubble_message);
	bubble_message.setLookAndFeel(&look_and_feel_v3);
	bubble_message.toFront(false);

	crosshair_colour = Colours::white;

	setWantsKeyboardFocus(true);

	VStr json_filenames;
	std::atomic<bool> done = false;
	find_files(File(project_info.project_dir), image_filenames, json_filenames, done);
	Log("number of images found in " + project_info.project_dir + ": " + std::to_string(image_filenames.size()));

	const std::string exclusion_regex = cfg().get_str(cfg_prefix + "exclusion_regex");
	if (exclusion_regex.empty() == false)
	{
		try
		{
			const std::regex rx(exclusion_regex);

			VStr v;
			for (auto && fn : image_filenames)
			{
				const bool result = std::regex_search(fn, rx);
				if (result == false)
				{
					v.push_back(fn);
				}
			}

			if (v.size() != image_filenames.size())
			{
				v.swap(image_filenames);
				AlertWindow::showMessageBoxAsync(AlertWindow::AlertIconType::InfoIcon, "DarkMark",
						"This project has an exclusion regex:\n\n"
						"\t\t" + exclusion_regex + "\n\n" +
						std::to_string(v.size() - image_filenames.size()) + " images were excluded by this filter, bringing the total number of images down from " +
						std::to_string(v.size()) + " to " + std::to_string(image_filenames.size()) + ".\n\n"
						"Clear the \"exclusion regex\" field in the launcher window to include all images in the project."
						);
			}
		}
		catch (...)
		{
			AlertWindow::showMessageBoxAsync(AlertWindow::AlertIconType::WarningIcon, "DarkMark", "The \"exclusion regex\" for this project has caused an error and has been skipped.");
		}
	}

	set_sort_order(sort_order);

	return;
}


dm::DMContent::~DMContent(void)
{
	if (need_to_save)
	{
		save_json();
		save_text();
	}

	return;
}


void dm::DMContent::resized()
{
	const double window_width	= getWidth();
	const double window_height	= getHeight();
	if(	window_width	< 1.0 or
		window_height	< 1.0 )
	{
		// window hasn't been created yet?
		return;
	}

	double image_width	= original_image.cols;
	double image_height	= original_image.rows;
	if (image_width		< 1.0 or
		image_height	< 1.0 )
	{
		// image hasn't been loaded yet?
		image_width		= 640;
		image_height	= 480;
	}

	// determine the size of the image once it is scaled
	const double min_horizontal_spacer_height	= (scrollfield_width > 0 ? 2.0 : 0.0);
	const double width_ratio					= (window_width - min_horizontal_spacer_height - scrollfield_width) / image_width;
	const double height_ratio					= window_height / image_height;
	const double ratio							= std::min(width_ratio, height_ratio);
	const double new_image_width				= std::round(ratio * image_width);
	const double new_image_height				= std::round(ratio * image_height);

	canvas.setBounds(0, 0, new_image_width, new_image_height);
	scrollfield.setBounds(window_width - scrollfield_width, 0, scrollfield_width, window_height);

	// remember some of the important numbers so we don't have to re-calculate them later
	scaled_image_size = cv::Size(new_image_width, new_image_height);
	scale_factor = ratio;

	// update the window title to show the scale factor
	if (dmapp().wnd)
	{
		// since we're going to be messing around with the window title, make a copy of the original window name

		static const std::string original_title = dmapp().wnd->getName().toStdString();

		std::string title =
			original_title +
			" - "	+ std::to_string(1 + image_filename_index) + "/" + std::to_string(image_filenames.size()) +
			" - "	+ short_filename +
			" - "	+ std::to_string(original_image.cols) +
			"x"		+ std::to_string(original_image.rows) +
			" - "	+ std::to_string(static_cast<int>(std::round(scale_factor * 100.0))) + "%";

		dmapp().wnd->setName(title);
	}

	return;
}


void dm::DMContent::start_darknet()
{
	Log("loading darknet neural network");
	const std::string darknet_cfg		= cfg().get_str(cfg_prefix + "cfg"		);
	const std::string darknet_weights	= cfg().get_str(cfg_prefix + "weights"	);
	const std::string darknet_names		= cfg().get_str(cfg_prefix + "names"	);
	names.clear();

	if (darknet_cfg		.empty() == false	and
		darknet_weights	.empty() == false	and
		File(darknet_cfg).existsAsFile()	and
		File(darknet_weights).existsAsFile())
	{
		try
		{
			dmapp().darkhelp.reset(new DarkHelp(darknet_cfg, darknet_weights, darknet_names));
			Log("neural network loaded in " + darkhelp().duration_string());

			darkhelp().threshold							= cfg().get_int("darknet_threshold")			/ 100.0f;
			darkhelp().hierarchy_threshold					= cfg().get_int("darknet_hierarchy_threshold")	/ 100.0f;
			darkhelp().non_maximal_suppression_threshold	= cfg().get_int("darknet_nms_threshold")		/ 100.0f;
			names = darkhelp().names;
		}
		catch (const std::exception & e)
		{
			dmapp().darkhelp.reset(nullptr);
			Log("failed to load darknet (cfg=" + darknet_cfg + ", weights=" + darknet_weights + ", names=" + darknet_names + "): " + e.what());
			AlertWindow::showMessageBoxAsync(
				AlertWindow::AlertIconType::WarningIcon,
				"DarkMark",
				"Failed to load darknet neural network. The error message returned was:\n" +
				String("\n") +
				e.what());
		}
	}
	else
	{
		Log("skipped loading darknet due to missing or invalid .cfg or .weights filenames");
		AlertWindow::showMessageBoxAsync(
			AlertWindow::AlertIconType::InfoIcon,
			"DarkMark",
			"One or more required neural network file was not found. The neural network cannot be loaded.");
	}

	if (names.empty() and darknet_names.empty() == false)
	{
		Log("manually parsing " + darknet_names);
		std::ifstream ifs(darknet_names);
		std::string line;
		while (std::getline(ifs, line))
		{
			if (line.empty())
			{
				break;
			}
			names.push_back(line);
		}
	}
	if (names.empty())
	{
		Log("classes/names is empty -- creating some dummy entries");
		names = { "car", "person", "bicycle", "dog", "cat" };
	}

	Log("number of name entries: " + std::to_string(names.size()));

	// add 1 more special entry to the end of the "names" so we can deal with empty images
	empty_image_name_index = names.size();
	names.push_back("* empty image *");

	annotation_colours = DarkHelp::get_default_annotation_colours();
	if (annotation_colours.empty() == false)
	{
		const auto & opencv_colour = annotation_colours.at(most_recent_class_idx);
		crosshair_colour = Colour(opencv_colour[2], opencv_colour[1], opencv_colour[0]);
	}

	load_image(0);

	return;
}


void dm::DMContent::rebuild_image_and_repaint()
{
	canvas.need_to_rebuild_cache_image = true;
	canvas.repaint();

	if (scrollfield_width > 0)
	{
		scrollfield.draw_marker_at_current_image();
	}

	return;
}


bool dm::DMContent::keyPressed(const KeyPress &key)
{
//	Log("code=" + std::to_string(key.getKeyCode()) + " char=" + std::to_string(key.getTextCharacter()) + " description=" + key.getTextDescription().toStdString());

	const auto keycode = key.getKeyCode();
	const auto keychar = key.getTextCharacter();

	const KeyPress key0 = KeyPress::createFromDescription("0");
	const KeyPress key9 = KeyPress::createFromDescription("9");

	int digit = -1;
	if (keycode >= key0.getKeyCode() and keycode <= key9.getKeyCode())
	{
		digit = keycode - key0.getKeyCode();
	}

	if (keycode == KeyPress::tabKey)
	{
		if (marks.empty())
		{
			selected_mark = -1;
		}
		else
		{
			int attempt = 10;
			while (attempt >= 0)
			{
				if (key.getModifiers().isShiftDown())
				{
					// select previous mark
					selected_mark --;
					if (selected_mark < 0 or selected_mark >= (int)marks.size())
					{
						// wrap back around to the last mark
						selected_mark = marks.size() - 1;
					}
				}
				else
				{
					// select next mark
					selected_mark ++;
					if (selected_mark < 0 or selected_mark >= (int)marks.size())
					{
						// wrap back around to the very first mark
						selected_mark = 0;
					}
				}

				const auto & m = marks.at(selected_mark);
				if ((marks_are_shown and m.is_prediction == false) or
					(predictions_are_shown and m.is_prediction))
				{
					// we found one that works!  keep it!
					break;
				}

				// try again to find a mark that is shown on the screen
				attempt --;
			}
			if (attempt < 0)
			{
				selected_mark = -1;
			}
		}

		if (selected_mark >= 0)
		{
			// remember the class and size of this mark in case the user wants to double-click and create a similar one
			const Mark & m = marks.at(selected_mark);
			most_recent_class_idx = m.class_idx;
			most_recent_size = m.get_normalized_bounding_rect().size();

			const auto & opencv_colour = annotation_colours.at(most_recent_class_idx);
			crosshair_colour = Colour(opencv_colour[2], opencv_colour[1], opencv_colour[0]);
		}

		rebuild_image_and_repaint();
		return true; // event has been handled
	}
	else if (digit >= 0 and digit <= 9)
	{
		if (key.getModifiers().isCtrlDown())
		{
			digit += 10;
		}
		if (key.getModifiers().isAltDown())
		{
			digit += 20;
		}

		// change the class for the selected mark
		set_class(digit);
		return true; // event has been handled
	}
	else if (keycode == KeyPress::homeKey)
	{
		load_image(0);
		return true;
	}
	else if (keycode == KeyPress::endKey)
	{
		load_image(image_filenames.size() - 1);
		return true;
	}
	else if (keycode == KeyPress::rightKey)
	{
		if (image_filename_index < image_filenames.size() - 1)
		{
			load_image(image_filename_index + 1);
		}
		return true;
	}
	else if (keycode == KeyPress::leftKey)
	{
		if (image_filename_index > 0)
		{
			load_image(image_filename_index - 1);
		}
		return true;
	}
	else if (keycode == KeyPress::pageUpKey)
	{
		// go to the previous available image with no marks
		auto idx = image_filename_index;
		while (idx > 0)
		{
			idx --;

			File f(image_filenames[idx]);
			f = f.withFileExtension(".json");
			if (count_marks_in_json(f) == 0)
			{
				break;
			}
		}
		load_image(idx);
		return true;

	}
	else if (keycode == KeyPress::pageDownKey)
	{
		// go to the next available image with no marks
		auto idx = image_filename_index;
		while (idx < image_filenames.size() - 1)
		{
			idx ++;

			File f(image_filenames[idx]);
			f = f.withFileExtension(".json");
			if (count_marks_in_json(f) == 0)
			{
				break;
			}
		}
		load_image(idx);
		return true;
	}
	else if (keycode == KeyPress::upKey or keycode == KeyPress::downKey)
	{
		if (dmapp().darkhelp)
		{
			float threshold = dmapp().darkhelp->threshold;

			threshold += (keycode == KeyPress::upKey ? 0.05f : -0.05f);
			threshold = std::min(std::max(threshold, 0.05f), 0.95f);

			if (threshold != dmapp().darkhelp->threshold)
			{
				dmapp().darkhelp->threshold = threshold;
				load_image(image_filename_index);
				show_message("darknet threshold: " + std::to_string((int)std::round(100.0 * threshold)) + "%");
			}
		}
		return true;
	}
	else if (keycode == KeyPress::deleteKey or keycode == KeyPress::backspaceKey or keycode == KeyPress::numberPadDelete)
	{
		if (selected_mark >= 0)
		{
			auto iter = marks.begin() + selected_mark;
			marks.erase(iter);
			selected_mark = -1;
			need_to_save = true;
			rebuild_image_and_repaint();
			return true;
		}
	}
	else if (keycode == KeyPress::escapeKey)
	{
		dmapp().wnd->closeButtonPressed();
	}
	else if (keycode == KeyPress::F1Key)
	{
		if (not dmapp().about_wnd)
		{
			dmapp().about_wnd.reset(new AboutWnd);
		}
		dmapp().about_wnd->toFront(true);
		return true;
	}
	else if (keychar == 'r')
	{
		set_sort_order(ESort::kRandom);
		show_message("re-shuffle random sort");
		return true;
	}
	else if (keychar == 'a')
	{
		accept_all_marks();
		return true; // event has been handled
	}
	else if (keychar == 'p')
	{
		EToggle toggle = static_cast<EToggle>( (int(show_predictions) + 1) % 3 );
		toggle_show_predictions(toggle);
		show_message("predictions: " + std::string(
				toggle == EToggle::kOn	? "on"	:
				toggle == EToggle::kOff	? "off"	: "auto"));

		return true;
	}
	else if (keychar == 'm')
	{
		toggle_show_marks();
		show_message("user marks: " + std::string(show_marks ? "visible" : "hidden"));
		return true;
	}
	else if (keychar == 'l')
	{
		EToggle toggle = static_cast<EToggle>( (int(show_labels) + 1) % 3 );
		set_labels(toggle);
		show_message("labels: " + std::string(
				toggle == EToggle::kOn	? "on"	:
				toggle == EToggle::kOff	? "off"	: "auto"));
		return true;
	}
	else if (keychar == 'b')
	{
		toggle_bold_labels();
		show_message("bold: " + std::string(all_marks_are_bold ? "enable" : "disable"));
		return true;
	}
	else if (keychar == 'B')
	{
		toggle_shade_rectangles();
		show_message("shade: " + std::string(shade_rectangles ? "enable" : "disable"));
		return true;
	}
	else if (keychar == 'c' or keycode == KeyPress::returnKey)
	{
		create_class_menu().showMenuAsync(PopupMenu::Options());
		return true;
	}
	else if (keychar == 'j')
	{
		show_jump_wnd();
		return true;
	}
	else if (keychar == 's')
	{
		save_screenshot(false);
		return true;
	}
	else if (keychar == 'S')
	{
		save_screenshot(true);
		return true;
	}
	else if (keychar == 'y')
	{
		copy_marks_from_previous_image();
		return true;
	}
	else if (keychar == 'e')
	{
		if (not dmapp().settings_wnd)
		{
			dmapp().settings_wnd.reset(new SettingsWnd(*this));
		}
		dmapp().settings_wnd->toFront(true);
	}
	#if 0
	else if (keychar == 'z')
	{
		const auto old_image_index = image_filename_index;
		show_predictions = EToggle::kOn;
		show_marks = false;
		for (size_t idx = 0; idx < image_filenames.size(); idx ++)
		{
			load_image(idx);
			std::stringstream ss;
			ss << "frame_" << std::setfill('0') << std::setw(4) << idx << ".png";
			save_screenshot(true, ss.str());
		}
		load_image(old_image_index);
	}
	#endif
	else
	{
		show_message("ignoring unknown key '" + key.getTextDescription().toStdString() + "'");
	}

	return false;
}


dm::DMContent & dm::DMContent::set_class(const size_t class_idx)
{
	if (selected_mark >= 0 and (size_t)selected_mark < marks.size())
	{
		if (class_idx >= names.size() - 1)
		{
			Log("class idx \"" + std::to_string(class_idx) + "\" is beyond the last index");
			AlertWindow::showMessageBox(AlertWindow::AlertIconType::WarningIcon, "DarkMark", "Class id #" + std::to_string(class_idx) + " is beyond the highest class defined in " + cfg().get_str(cfg_prefix + "names") + ".");
		}
		else
		{
			auto & m = marks[selected_mark];
			m.class_idx = class_idx;
			m.name = names.at(m.class_idx);
			m.description = names.at(m.class_idx);
			need_to_save = true;
		}
	}

	if (class_idx < names.size() - 1)
	{
		most_recent_class_idx = class_idx;
		const auto & opencv_colour = annotation_colours.at(most_recent_class_idx);
		crosshair_colour = Colour(opencv_colour[2], opencv_colour[1], opencv_colour[0]);
		rebuild_image_and_repaint();
	}

	return *this;
}


dm::DMContent & dm::DMContent::set_sort_order(const dm::ESort new_sort_order)
{
	if (sort_order != new_sort_order)
	{
		sort_order = new_sort_order;
		const int tmp = static_cast<int>(sort_order);

		Log("changing sort order to #" + std::to_string(tmp));
		cfg().setValue("sort_order", tmp);
	}

	if (image_filenames.empty())
	{
		return *this;
	}

	// remember the current image filename so we can scroll back to the same one once we're done sorting
	const std::string old_filename = image_filenames.at(image_filename_index);

	switch (sort_order)
	{
		case ESort::kRandom:
		{
			std::random_shuffle(image_filenames.begin(), image_filenames.end());
			break;
		}
		case ESort::kCountMarks:
		{
			// this one takes a while, so start a progress thread to do the work
			DMContentImageFilenameSort helper(*this);
			helper.runThread();
			break;
		}
		case ESort::kTimestamp:
		{
			// this one takes a while, so start a progress thread to do the work
			DMContentImageFilenameSort helper(*this);
			helper.runThread();
			break;
		}
		case ESort::kAlphabetical:
		default:
		{
			std::sort(image_filenames.begin(), image_filenames.end());
			break;
		}
	}

	size_t idx = 0;
	if (sort_order != ESort::kRandom)
	{
		// as long as the sort order isn't random, then find the previous image within the newly sorted images
		for (idx = 0; idx < image_filenames.size(); idx ++)
		{
			if (old_filename == image_filenames[idx])
			{
				break;
			}
		}
	}
	load_image(idx);

	if (scrollfield_width > 0)
	{
		scrollfield.rebuild_entire_field_on_thread();
	}

	return *this;
}


dm::DMContent & dm::DMContent::set_labels(const EToggle toggle)
{
	if (show_labels != toggle)
	{
		show_labels = toggle;
		cfg().setValue("show_labels", static_cast<int>(show_labels));
		rebuild_image_and_repaint();
	}

	return *this;
}


dm::DMContent & dm::DMContent::toggle_shade_rectangles()
{
	shade_rectangles = not shade_rectangles;
	cfg().setValue("shade_rectangles", shade_rectangles);
	rebuild_image_and_repaint();

	return *this;
}


dm::DMContent & dm::DMContent::toggle_bold_labels()
{
	all_marks_are_bold = not all_marks_are_bold;
	cfg().setValue("all_marks_are_bold", all_marks_are_bold);
	rebuild_image_and_repaint();

	return *this;
}


dm::DMContent & dm::DMContent::toggle_show_predictions(const EToggle toggle)
{
	if (show_predictions != toggle)
	{
		show_predictions = toggle;
		cfg().setValue("show_predictions", static_cast<int>(show_predictions));
	}

	// rebuilding the cache image isn't enough here, we need to completely reload the image so darknet can process the image
	load_image(image_filename_index);

	return *this;
}


dm::DMContent & dm::DMContent::toggle_show_marks()
{
	show_marks = not show_marks;

	cfg().setValue("show_marks", show_marks);

	rebuild_image_and_repaint();

	return *this;
}


dm::DMContent & dm::DMContent::toggle_show_processing_time()
{
	show_processing_time = not show_processing_time;

	cfg().setValue("show_processing_time", show_processing_time);

	rebuild_image_and_repaint();

	return *this;
}


dm::DMContent & dm::DMContent::load_image(const size_t new_idx, const bool full_load)
{
//	if (marks.empty() == false)
	if (need_to_save)
	{
		save_json();
		save_text();
	}

	darknet_image_processing_time = "";
	selected_mark	= -1;
	original_image	= cv::Mat();
	marks.clear();
	image_is_completely_empty = false;

	if (new_idx >= image_filenames.size())
	{
		image_filename_index = image_filenames.size() - 1;
	}
	else
	{
		image_filename_index = new_idx;
	}
	long_filename	= image_filenames.at(image_filename_index);
	short_filename	= File(long_filename).getFileName().toStdString();
	json_filename	= File(long_filename).withFileExtension(".json"	).getFullPathName().toStdString();
	text_filename	= File(long_filename).withFileExtension(".txt"	).getFullPathName().toStdString();

	if (dmapp().jump_wnd)
	{
		Slider & slider = dmapp().jump_wnd->slider;
		slider.setValue(image_filename_index + 1);
	}

	bool exception_caught = false;
	std::string what_msg;
	std::string task = "[unknown]";
	try
	{
		task = "loading image file " + long_filename;
		Log("loading image " + long_filename);
		original_image = cv::imread(image_filenames.at(image_filename_index));

		if (full_load)
		{
			task = "loading json file " + json_filename;
			bool success = load_json();
			if (not success)
			{
				// only attempt to load the .txt file if there was no .json file to process
				task = "importing text file " + text_filename;
				success = load_text();
			}

			if (success and (File(json_filename).existsAsFile() != File(text_filename).existsAsFile()))
			{
				// we either have the .json without the .txt, or the other way around, in which case we need to re-save the files
				need_to_save = true;
			}

			if (show_predictions != EToggle::kOff)
			{
				if (dmapp().darkhelp)
				{
					task = "getting predictions";
					darkhelp().predict(original_image);
					darknet_image_processing_time = darkhelp().duration_string();
					Log("darkhelp processed " + short_filename + " in " + darknet_image_processing_time);

//					std::cout << darkhelp().prediction_results << std::endl;

					// convert the predictions into marks
					task = "converting predictions";
					for (auto prediction : darkhelp().prediction_results)
					{
						Mark m(prediction.original_point, prediction.original_size, original_image.size(), prediction.best_class);
						m.name = names.at(m.class_idx);
						m.description = prediction.name;
						m.is_prediction = true;
						marks.push_back(m);
					}
				}
			}

			// Sort the marks based on a gross (rounded) X and Y position of the midpoint.  This way when
			// the user presses TAB or SHIFT+TAB the marks appear in a consistent and predictable order.
			task = "sorting marks";
			std::sort(marks.begin(), marks.end(),
					[](auto & lhs, auto & rhs)
					{
						const auto & p1 = lhs.get_normalized_midpoint();
						const auto & p2 = rhs.get_normalized_midpoint();

						const int y1 = std::round(15.0 * p1.y);
						const int y2 = std::round(15.0 * p2.y);

						if (y1 < y2) return true;
						if (y2 < y1) return false;

						// if we get here then y1 and y2 are the same, so now we compare x1 and x2

						const int x1 = std::round(15.0 * p1.x);
						const int x2 = std::round(15.0 * p2.x);

						if (x1 < x2) return true;

						return false;
					} );
		}
	}
	catch(const std::exception & e)
	{
		exception_caught = true;
		Log("Error: exception caught while " + task + ": " + e.what());
		what_msg = e.what();
	}
	catch(...)
	{
		exception_caught = true;
		Log("Error: failed while " + task);
		what_msg = "\"unknown\"";
	}

	if (exception_caught)
	{
		original_image = cv::Mat(32, 32, CV_8UC3, cv::Scalar(0, 0, 255)); // use a red square to indicate a problem
		AlertWindow::showMessageBoxAsync(
			AlertWindow::AlertIconType::WarningIcon,
			"DarkMark",
			"Failure occurred while " + task + ". See log file for details.\n"
			"\n"
			"The most likely cause of this failure is when Darknet has been recently updated, but the version of DarkHelp installed is for an older version of libdarknet. If this is the case, then rebuilding DarkHelp should fix the issue.\n"
			"\n"
			"The exact error message logged is: " + what_msg);
	}

	resized();
	rebuild_image_and_repaint();

	return *this;
}


dm::DMContent & dm::DMContent::save_text()
{
	if (text_filename.empty() == false)
	{
		bool delete_txt_file = true;

		if (image_is_completely_empty)
		{
			delete_txt_file = false;
		}

		std::ofstream fs(text_filename);
		for (const auto & m : marks)
		{
			if (m.is_prediction)
			{
				// skip this one since it is a prediction, not a full mark
				continue;
			}

			delete_txt_file = false;

			const cv::Rect2d r	= m.get_normalized_bounding_rect();
			const double w		= r.width;
			const double h		= r.height;
			const double x		= r.x + w / 2.0;
			const double y		= r.y + h / 2.0;
			fs << std::fixed << std::setprecision(10) << m.class_idx << " " << x << " " << y << " " << w << " " << h << std::endl;
		}

		fs.close();

		if (delete_txt_file)
		{
			// there was no legitimate reason to keep the .txt file
			std::remove(text_filename.c_str());
		}
	}

	return *this;
}


dm::DMContent & dm::DMContent::save_json()
{
	if (json_filename.empty() == false)
	{
		json root;
		size_t next_id = 0;
		for (auto & m : marks)
		{
			if (m.is_prediction)
			{
				// skip this one since it is a prediction, not a full mark
				continue;
			}

			root["mark"][next_id]["class_idx"	] = m.class_idx;
			root["mark"][next_id]["name"		] = m.name;

			const cv::Rect2d	r1 = m.get_normalized_bounding_rect();
			const cv::Rect		r2 = m.get_bounding_rect(original_image.size());

			root["mark"][next_id]["rect"]["x"]		= r1.x;
			root["mark"][next_id]["rect"]["y"]		= r1.y;
			root["mark"][next_id]["rect"]["w"]		= r1.width;
			root["mark"][next_id]["rect"]["h"]		= r1.height;
			root["mark"][next_id]["rect"]["int_x"]	= r2.x;
			root["mark"][next_id]["rect"]["int_y"]	= r2.y;
			root["mark"][next_id]["rect"]["int_w"]	= r2.width;
			root["mark"][next_id]["rect"]["int_h"]	= r2.height;

			for (size_t point_idx = 0; point_idx < m.normalized_all_points.size(); point_idx ++)
			{
				const cv::Point2d & p = m.normalized_all_points.at(point_idx);
				root["mark"][next_id]["points"][point_idx]["x"] = p.x;
				root["mark"][next_id]["points"][point_idx]["y"] = p.y;

				// DarkMark doesn't use these integer values, but make them available for 3rd party software which wants to reads the .json file
				root["mark"][next_id]["points"][point_idx]["int_x"] = (int)(std::round(p.x * (double)original_image.cols));
				root["mark"][next_id]["points"][point_idx]["int_y"] = (int)(std::round(p.y * (double)original_image.rows));
			}

			next_id ++;
		}
		root["image"]["scale"]	= scale_factor;
		root["image"]["width"]	= original_image.cols;
		root["image"]["height"]	= original_image.rows;
		root["timestamp"]		= std::time(nullptr);
		root["version"]			= DARKMARK_VERSION;

		if (next_id == 0 and image_is_completely_empty)
		{
			// no marks were written out, so this must be an empty image
			root["completely_empty"] = true;
		}
		else
		{
			root["completely_empty"] = false;
		}

		if (next_id > 0 or image_is_completely_empty)
		{
			std::ofstream fs(json_filename);
			fs << root.dump(1, '\t') << std::endl;
		}
		else
		{
			// image has no markup -- delete the .json file if it existed
			std::remove(json_filename.c_str());
		}

		if (scrollfield_width > 0)
		{
			scrollfield.update_index(image_filename_index);
			scrollfield.need_to_rebuild_cache_image = true;
		}
	}

	need_to_save = false;

	return *this;
}


size_t dm::DMContent::count_marks_in_json(File & f)
{
	size_t result = 0;

	if (f.existsAsFile())
	{
		try
		{
			json root = json::parse(f.loadFileAsString().toStdString());
			result = root["mark"].size();

			if (result == 0 and root.value("completely_empty", false))
			{
				// if there are zero marks, then see if the image has been identified
				// as completely empty, and if so count that as if it was a mark
				result = 1;
			}
		}
		catch (const std::exception & e)
		{
			AlertWindow::showMessageBox(
				AlertWindow::AlertIconType::WarningIcon,
				"DarkMark",
				"Failed to read or parse the .json file " + f.getFullPathName().toStdString() + ":\n"
				"\n" +
				e.what());
		}
	}

	return result;
}


bool dm::DMContent::load_text()
{
	bool success = false;

	File f(text_filename);
	if (f.existsAsFile())
	{
		success = true;
		StringArray sa;
		f.readLines(sa);
		sa.removeEmptyStrings();
		for (auto iter = sa.begin(); iter != sa.end(); iter ++)
		{
			std::stringstream ss(iter->toStdString());
			int class_idx = 0;
			double x = 0.0;
			double y = 0.0;
			double w = 0.0;
			double h = 0.0;
			ss >> class_idx >> x >> y >> w >> h;
			Mark m(cv::Point2d(x, y), cv::Size2d(w, h), cv::Size(0, 0), class_idx);
			m.name = names.at(class_idx);
			m.description = m.name;
			marks.push_back(m);
		}

		if (marks.empty())
		{
			image_is_completely_empty = true;
		}
	}

	return success;
}


bool dm::DMContent::load_json()
{
	bool success = false;

	File f(json_filename);
	if (f.existsAsFile())
	{
		json root = json::parse(f.loadFileAsString().toStdString());

		for (size_t idx = 0; idx < root["mark"].size(); idx ++)
		{
			Mark m;
			m.class_idx = root["mark"][idx]["class_idx"];
			m.name = root["mark"][idx]["name"];
			m.description = m.name;
			m.normalized_all_points.clear();
			for (size_t point_idx = 0; point_idx < root["mark"][idx]["points"].size(); point_idx ++)
			{
				cv::Point2d p;
				p.x = root["mark"][idx]["points"][point_idx]["x"];
				p.y = root["mark"][idx]["points"][point_idx]["y"];
				m.normalized_all_points.push_back(p);
			}
			m.rebalance();
			marks.push_back(m);
		}

		if (marks.empty())
		{
			image_is_completely_empty = root.value("completely_empty", false);
		}

		success = true;
	}

	return success;
}


dm::DMContent & dm::DMContent::show_darknet_window()
{
	if (not dmapp().darknet_wnd)
	{
		dmapp().darknet_wnd.reset(new DarknetWnd(*this));
	}
	dmapp().darknet_wnd->toFront(true);

	return *this;
}


dm::DMContent & dm::DMContent::delete_current_image()
{
	if (image_filename_index < image_filenames.size())
	{
		File f(image_filenames[image_filename_index]);
		Log("deleting the file at index #" + std::to_string(image_filename_index) + ": " + f.getFullPathName().toStdString());
		f.deleteFile();
		f.withFileExtension(".txt"	).deleteFile();
		f.withFileExtension(".json"	).deleteFile();
		image_filenames.erase(image_filenames.begin() + image_filename_index);
		load_image(image_filename_index);
	}

	return *this;
}


dm::DMContent & dm::DMContent::copy_marks_from_previous_image()
{
	// first we need to make a copy of the image list and sort it alphabetically;
	// this helps us identify exactly which image is "previous" (assuming images are numbered!)
	auto alphabetical_image_filenames = image_filenames;
	std::sort(alphabetical_image_filenames.begin(), alphabetical_image_filenames.end());

	// find the current index within the alphabetical list
	size_t idx;
	for (idx = 0; idx < alphabetical_image_filenames.size(); idx ++)
	{
		if (alphabetical_image_filenames[idx] == image_filenames[image_filename_index])
		{
			break;
		}
	}

	size_t count_added = 0;
	size_t count_skipped = 0;

	// now go through all the previous images one at a time until we find one that has marks we can copy
	while (true)
	{
		if (idx == 0)
		{
			// there is no more previous images -- nothing we can do
			break;
		}

		idx --;

		File f = File(alphabetical_image_filenames[idx]).withFileExtension(".json");
		if (f.existsAsFile() == false)
		{
			// keep looking
			continue;
		}

		json root;
		try
		{
			root = json::parse(f.loadFileAsString().toStdString());
		}
		catch (const std::exception & e)
		{
			// ignore the error, nothing we can do about broken .json files
		}

		if (root["mark"].empty())
		{
			// no marks, so keep looking for a better image we can use
			continue;
		}

		// if we get here then we've found the "previous" image from which we'll copy marks

		// re-create the marks from the content of the .json file
		for (auto m : root["mark"])
		{
			Mark new_mark;
			new_mark.class_idx		= m["class_idx"];
			new_mark.name			= m["name"];
			new_mark.description	= new_mark.name;
			new_mark.normalized_all_points.clear();
			for (size_t point_idx = 0; point_idx < m["points"].size(); point_idx ++)
			{
				cv::Point2d p;
				p.x = m["points"][point_idx]["x"];
				p.y = m["points"][point_idx]["y"];
				new_mark.normalized_all_points.push_back(p);
			}
			new_mark.rebalance();

			// check to see if we already have this mark
			bool already_exists = false;
			for (const auto & old_mark : marks)
			{
				if (old_mark.normalized_corner_points == new_mark.normalized_corner_points && old_mark.class_idx == new_mark.class_idx)
				{
					count_skipped ++;
					already_exists = true;
					break;
				}
			}
			if (already_exists == false)
			{
				marks.push_back(new_mark);
				count_added ++;
			}
		}

		std::stringstream ss;
		if (count_added)
		{
			ss << "copied " << count_added << " mark" << (count_added == 1 ? "" : "s");
		}
		if (count_skipped)
		{
			if (count_added)
			{
				ss << " and ";
			}

			ss << "skipped " << count_skipped << " identical mark" << (count_skipped == 1 ? "" : "s");
		}
		ss << " from " << File(alphabetical_image_filenames[idx]).getFileName().toStdString();

		show_message(ss.str());

		if (count_added)
		{
			need_to_save = true;
			rebuild_image_and_repaint();
		}

		// we've now copied over all the old marks we need, so stop looking at previous images
		break;
	}

	if (count_added == 0 && count_skipped == 0)
	{
		show_message("no previous images with marks were found");
	}

	return *this;
}


dm::DMContent & dm::DMContent::accept_all_marks()
{
	for (auto & m : marks)
	{
		m.is_prediction	= false;
		m.name			= names.at(m.class_idx);
		m.description	= names.at(m.class_idx);
	}

	need_to_save = true;
	rebuild_image_and_repaint();

	return *this;
}


dm::DMContent & dm::DMContent::erase_all_marks()
{
	Log("deleting all marks for " + long_filename);

	marks.clear();
	need_to_save = false;
	File(json_filename).deleteFile();
	File(text_filename).deleteFile();
	load_image(image_filename_index);
	rebuild_image_and_repaint();

	return *this;
}


PopupMenu dm::DMContent::create_class_menu()
{
	const bool is_enabled = (selected_mark >= 0 and (size_t)selected_mark < marks.size() ? true : false);

	int selected_class_idx = -1;
	if (is_enabled)
	{
		const Mark & m = marks.at(selected_mark);
		selected_class_idx = (int)m.class_idx;
	}

	PopupMenu m;
	for (size_t idx = 0; idx < names.size() - 1; idx ++)
	{
		const std::string & name = std::to_string(idx) + " - " + names.at(idx);

		const bool is_ticked = (selected_class_idx == (int)idx ? true : false);

		if (idx % 10 == 0 and names.size() - 1 > 1)
		{
			std::stringstream ss;
			if (idx == 10) ss << "CTRL + ";
			if (idx == 20) ss << "ALT + ";
			if (idx == 30) ss << "CTRL + ALT + ";
			ss << "0";
			const size_t max_val = std::min(names.size() - 2, idx + 9) - idx;
			if (max_val > 0)
			{
				ss << " to " << max_val;
			}
			m.addSectionHeader(ss.str());
		}

		m.addItem(name, (is_enabled and not is_ticked), is_ticked, std::function<void()>( [&, idx]{ this->set_class(idx); } ));
	}

	bool image_already_marked = false;
	for (const auto & mark : marks)
	{
		if (mark.is_prediction == false)
		{
			image_already_marked = true;
			image_is_completely_empty = false;
			break;
		}
	}

	m.addSeparator();
	m.addItem("empty image", (image_already_marked == false), image_is_completely_empty, std::function<void()>( [&]
	{
		image_is_completely_empty = ! image_is_completely_empty;
		rebuild_image_and_repaint();
		need_to_save = true;
	} ));

	return m;
}


PopupMenu dm::DMContent::create_popup_menu()
{
	PopupMenu classMenu = create_class_menu();

	PopupMenu labels;
	labels.addItem("always show labels"	, (show_labels != EToggle::kOn	), (show_labels == EToggle::kOn		), std::function<void()>( [&]{ set_labels(EToggle::kOn);	} ));
	labels.addItem("never show labels"	, (show_labels != EToggle::kOff	), (show_labels == EToggle::kOff	), std::function<void()>( [&]{ set_labels(EToggle::kOff);	} ));
	labels.addItem("auto show labels"	, (show_labels != EToggle::kAuto), (show_labels == EToggle::kAuto	), std::function<void()>( [&]{ set_labels(EToggle::kAuto);	} ));
	labels.addSeparator();
	labels.addItem("bold", true, all_marks_are_bold, std::function<void()>( [&]{ toggle_bold_labels(); } ));

	PopupMenu sort;
	sort.addItem("sort alphabetically"				, true, (sort_order == ESort::kAlphabetical	), std::function<void()>( [&]{ set_sort_order(ESort::kAlphabetical	); } ));
	sort.addItem("sort by modification timestamp"	, true, (sort_order == ESort::kTimestamp	), std::function<void()>( [&]{ set_sort_order(ESort::kTimestamp		); } ));
	sort.addItem("sort by number of marks"			, true, (sort_order == ESort::kCountMarks	), std::function<void()>( [&]{ set_sort_order(ESort::kCountMarks	); } ));
	sort.addItem("sort randomly"					, true, (sort_order == ESort::kRandom		), std::function<void()>( [&]{ set_sort_order(ESort::kRandom		); } ));

	PopupMenu view;
	view.addItem("always show darknet predictions"	, (show_predictions != EToggle::kOn		), (show_predictions == EToggle::kOn	), std::function<void()>( [&]{ toggle_show_predictions(EToggle::kOn);	} ));
	view.addItem("never show darknet predictions"	, (show_predictions != EToggle::kOff	), (show_predictions == EToggle::kOff	), std::function<void()>( [&]{ toggle_show_predictions(EToggle::kOff);	} ));
	view.addItem("auto show darknet predictions"	, (show_predictions != EToggle::kAuto	), (show_predictions == EToggle::kAuto	), std::function<void()>( [&]{ toggle_show_predictions(EToggle::kAuto);	} ));
	view.addSeparator();
	view.addItem("show darknet processing time"		, (show_predictions != EToggle::kOff	), (show_processing_time				), std::function<void()>( [&]{ toggle_show_processing_time();			} ));
	view.addSeparator();
	view.addItem("show marks"						, true									, show_marks							,  std::function<void()>( [&]{ toggle_show_marks();						} ));
	view.addItem("shade"							, true									, shade_rectangles						,  std::function<void()>( [&]{ toggle_shade_rectangles();				} ));

	const size_t number_of_darknet_marks = [&]
	{
		size_t count = 0;
		for (const auto & m : marks)
		{
			if (m.is_prediction)
			{
				count ++;
			}
		}

		return count;
	}();

	const bool has_any_marks = (marks.size() > 0);

	PopupMenu image;
	image.addItem("accept " + std::to_string(number_of_darknet_marks) + " pending mark" + (number_of_darknet_marks == 1 ? "" : "s"), (number_of_darknet_marks > 0)	, false	, std::function<void()>( [&]{ accept_all_marks();			} ));

	std::string text = "erase 1 mark";
	if (marks.size() != 1)
	{
		text = "erase all " + std::to_string(marks.size()) + " marks";
	}
	image.addItem(text																					, has_any_marks, false	, std::function<void()>( [&]{ erase_all_marks();			} ));
	image.addItem("delete image from disk"																						, std::function<void()>( [&]{ delete_current_image();		} ));
	image.addSeparator();
	image.addItem("jump..."																										, std::function<void()>( [&]{ show_jump_wnd();				} ));
	image.addSeparator();
	image.addItem("rotate images..."																							, std::function<void()>( [&]{ rotate_every_image();			} ));
	image.addItem("re-load and re-save every image"																				, std::function<void()>( [&]{ reload_resave_every_image();	} ));

	PopupMenu m;
	m.addSubMenu("class", classMenu, classMenu.containsAnyActiveItems());
	m.addSubMenu("labels", labels);
	m.addSubMenu("sort", sort);
	m.addSubMenu("view", view);
	m.addSubMenu("image", image);
	m.addSeparator();
	m.addItem("review marks..."			, std::function<void()>( [&]{ review_marks();			} ));
	m.addItem("gather statistics..."	, std::function<void()>( [&]{ gather_statistics();		} ));
	m.addItem("create darknet files..."	, std::function<void()>( [&]{ show_darknet_window();	} ));
	m.addItem("other settings..."		, std::function<void()>( [&]
	{
		if (not dmapp().settings_wnd)
		{
			dmapp().settings_wnd.reset(new SettingsWnd(*this));
		}
		dmapp().settings_wnd->toFront(true);
	}));

	return m;
}


dm::DMContent & dm::DMContent::gather_statistics()
{
	if (need_to_save)
	{
		save_json();
		save_text();
	}

	DMContentStatistics helper(*this);
	helper.runThread();

	return *this;
}


dm::DMContent & dm::DMContent::review_marks()
{
	if (need_to_save)
	{
		save_json();
		save_text();
	}

	DMContentReview helper(*this);
	helper.runThread();

	return *this;
}


dm::DMContent & dm::DMContent::rotate_every_image()
{
	const int result = AlertWindow::showYesNoCancelBox(AlertWindow::QuestionIcon, "DarkMark",
		"This will rotate images 90, 180, and 270 degrees, and will also rotate and copy all existing marks for each new image. "
		"Only run this if the network you are training uses images that do not have an obvious top/bottom/left/right direction.\n"
		"\n"
		"Examples:\n"
		"\n"
		"- If you are training with dash cam images of vehicles on a road, having those images rotated sideways and upside down "
		"doesn't make sense.\n"
		"\n"
		"- If you are training with images taken through a microscope, those images typically wouldn't have a fixed orientation, and the "
		"network training would benefit from having additional marked up images.\n"
		"\n"
		"Proceed with the image rotations?",
		"rotate all images",
		"rotate marked up images",
		"cancel"
	);

	if (result > 0)
	{
		DMContentRotateImages helper(*this, (result == 0));
		helper.runThread();
	}

	return *this;
}


dm::DMContent & dm::DMContent::reload_resave_every_image()
{
	DMContentReloadResave helper(*this);
	helper.runThread();

	return *this;
}


dm::DMContent & dm::DMContent::show_jump_wnd()
{
	if (not dmapp().jump_wnd)
	{
		dmapp().jump_wnd.reset(new DMJumpWnd(*this));
	}
	dmapp().jump_wnd->toFront(true);

	return *this;
}


dm::DMContent & dm::DMContent::show_message(const std::string & msg)
{
	if (msg.empty())
	{
		bubble_message.setVisible(false);
	}
	else
	{
		const Rectangle<int> r(getWidth()/2, 1, 1, 1);
		bubble_message.showAt(r, AttributedString(msg), 4000, true, false);
	}

	return *this;
}


dm::DMContent & dm::DMContent::save_screenshot(const bool full_size, const std::string & fn)
{
	bool proceed = false;
	std::string filename = fn;
	if (filename.empty())
	{
		filename = File(long_filename).getFileNameWithoutExtension().toStdString();
		filename += "_annotated.png";
	}
	else
	{
		proceed = true;
	}

	File f = File::getSpecialLocation(File::SpecialLocationType::userDesktopDirectory).getChildFile(filename);
	if (fn.empty())
	{
		FileChooser chooser("Save annotated image to...", f, "*.png,*.jpg,*.jpeg");
		if (chooser.browseForFileToSave(true))
		{
			f = chooser.getResult();
			proceed = true;
		}
	}

	if (proceed)
	{
		const auto old_scaled_image_size = scaled_image_size;

		if (full_size) // uppercase 'S' means we should use the full-size image
		{
			// we want to save the full-size image, not the resized one we're currently viewing,
			// so swap out a few things, re-build the annotated image, and save *those* results
			scaled_image_size = original_image.size();
			canvas.rebuild_cache_image();
		}

		if (f.hasFileExtension(".png"))
		{
			cv::imwrite(f.getFullPathName().toStdString(), scaled_image, {CV_IMWRITE_PNG_COMPRESSION, 9});
		}
		else
		{
			cv::imwrite(f.getFullPathName().toStdString(), scaled_image, {CV_IMWRITE_JPEG_OPTIMIZE, 1, CV_IMWRITE_JPEG_QUALITY, 75});
		}

		if (scaled_image_size != old_scaled_image_size)
		{
			// now put back the scaled image we expect to be there
			scaled_image_size = old_scaled_image_size;
			canvas.rebuild_cache_image();
		}
	}

	return *this;
}
