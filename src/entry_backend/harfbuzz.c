#include <cairo/cairo.h>
#include <harfbuzz/hb-ft.h>
#include <math.h>
#include "harfbuzz.h"
#include "../entry.h"
#include "../log.h"
#include "../nelem.h"
#include "../unicode.h"
#include "../xmalloc.h"

/*
 * FreeType is normally compiled without error strings, so we have to do this
 * funky macro trick to get them. See <freetype/fterrors.h> for more details.
 */
#undef FTERRORS_H_
#define FT_ERRORDEF( e, v, s )  { e, s },
#define FT_ERROR_START_LIST     {
#define FT_ERROR_END_LIST       { 0, NULL } };

const struct {
	int err_code;
	const char *err_msg;
} ft_errors[] =

#include <freetype/fterrors.h>

#undef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#undef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static void rounded_rectangle(cairo_t *cr, uint32_t width, uint32_t height, uint32_t r)
{
	cairo_new_path(cr);

	/* Top-left */
	cairo_arc(cr, r, r, r, -M_PI, -M_PI_2);

	/* Top-right */
	cairo_arc(cr, width - r, r, r, -M_PI_2, 0);

	/* Bottom-right */
	cairo_arc(cr, width - r, height - r, r, 0, M_PI_2);

	/* Bottom-left */
	cairo_arc(cr, r, height - r, r, M_PI_2, M_PI);

	cairo_close_path(cr);
}

static const char *get_ft_error_string(int err_code)
{
	for (size_t i = 0; i < N_ELEM(ft_errors); i++) {
		if (ft_errors[i].err_code == err_code) {
			return ft_errors[i].err_msg;
		}
	}

	return "Unknown FT error";
}

/*
 * Cairo / FreeType use 72 Pts per inch, but Pango uses 96 DPI, so we have to
 * rescale for consistency.
 */
#define PT_TO_DPI (96.0 / 72.0)

/*
 * hb_buffer_clear_contents also clears some basic script information, so group
 * them here for convenience.
 */
static void setup_hb_buffer(hb_buffer_t *buffer)
{
	hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
	hb_buffer_set_script(buffer, HB_SCRIPT_LATIN);
	hb_buffer_set_language(buffer, hb_language_from_string("en", -1));
}


/*
 * Render a hb_buffer with Cairo, and return the extents of the rendered text
 * in Cairo units.
 */
static cairo_text_extents_t render_hb_buffer(cairo_t *cr, hb_buffer_t *buffer)
{
	cairo_save(cr);

	/*
	 * Cairo uses y-down coordinates, but HarfBuzz uses y-up, so we
	 * shift the text down by its ascent height to compensate.
	 */
	cairo_font_extents_t font_extents;
	cairo_font_extents(cr, &font_extents);
	cairo_translate(cr, 0, font_extents.ascent);

	unsigned int glyph_count;
	hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(buffer, &glyph_count);
	hb_glyph_position_t *glyph_pos = hb_buffer_get_glyph_positions(buffer, &glyph_count);
	cairo_glyph_t *cairo_glyphs = xmalloc(sizeof(cairo_glyph_t) * glyph_count);

	double x = 0;
	double y = 0;
	for (unsigned int i=0; i < glyph_count; i++) {
		/*
		 * The coordinates returned by HarfBuzz are in 26.6 fixed-point
		 * format, so we divide by 64.0 (2^6) to get floats.
		 */
		cairo_glyphs[i].index = glyph_info[i].codepoint;
		cairo_glyphs[i].x = x + glyph_pos[i].x_offset / 64.0;
		cairo_glyphs[i].y = y - glyph_pos[i].y_offset / 64.0;
		x += glyph_pos[i].x_advance / 64.0;
		y -= glyph_pos[i].y_advance / 64.0;
	}

	cairo_show_glyphs(cr, cairo_glyphs, glyph_count);

	cairo_text_extents_t extents;
	cairo_glyph_extents(cr, cairo_glyphs, glyph_count, &extents);

	/* Account for the shifted baseline in our returned text extents. */
	extents.y_bearing += font_extents.ascent;

	free(cairo_glyphs);

	cairo_restore(cr);

	return extents;
}

/*
 * Clear the harfbuzz buffer, shape some text and render it with Cairo,
 * returning the extents of the rendered text in Cairo units.
 */
static cairo_text_extents_t render_text(
		cairo_t *cr,
		struct entry_backend_harfbuzz *hb,
		const char *text)
{
	hb_buffer_clear_contents(hb->hb_buffer);
	setup_hb_buffer(hb->hb_buffer);
	hb_buffer_add_utf8(hb->hb_buffer, text, -1, 0, -1);
	hb_shape(hb->hb_font, hb->hb_buffer, hb->hb_features, hb->num_features);
	return render_hb_buffer(cr, hb->hb_buffer);
}


/*
 * Render some text with an optional background box, using settings from the
 * given theme.
 */
static cairo_text_extents_t render_text_themed(
		cairo_t *cr,
		struct entry_backend_harfbuzz *hb,
		const char *text,
		const struct text_theme *theme)
{
	cairo_font_extents_t font_extents;
	cairo_font_extents(cr, &font_extents);
	struct directional padding = theme->padding;

	/*
	 * I previously thought rendering the text to a group, measuring it,
	 * drawing the box on the main canvas and then drawing the group would
	 * be the most efficient way of doing this. I was wrong.
	 *
	 * It turns out to be much quicker to just draw the text to the canvas,
	 * paint over it with the box, and then draw the text again. This is
	 * fine, as long as the box is always bigger than the text (which it is
	 * unless the user sets some extreme values for the corner radius).
	 */
	struct color color = theme->foreground_color;
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
	cairo_text_extents_t extents = render_text(cr, hb, text);

	if (theme->background_color.a == 0) {
		/* No background to draw, we're done. */
		return extents;
	}

	cairo_save(cr);
	color = theme->background_color;
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
	cairo_translate(
			cr,
			floor(-padding.left + extents.x_bearing),
			-padding.top);
	rounded_rectangle(
			cr,
			ceil(extents.width + padding.left + padding.right),
			ceil(font_extents.height + padding.top + padding.bottom),
			theme->background_corner_radius
			);
	cairo_fill(cr);
	cairo_restore(cr);

	color = theme->foreground_color;
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
	render_text(cr, hb, text);
	return extents;
}

static bool size_overflows(struct entry *entry, uint32_t width, uint32_t height)
{
	cairo_t *cr = entry->cairo[entry->index].cr;
	cairo_matrix_t mat;
	cairo_get_matrix(cr, &mat);
	if (entry->horizontal) {
		if (mat.x0 + width > entry->clip_x + entry->clip_width) {
			return true;
		}
	} else {
		if (mat.y0 + height > entry->clip_y + entry->clip_height) {
			return true;
		}
	}
	return false;
}

void entry_backend_harfbuzz_init(
		struct entry *entry,
		uint32_t *width,
		uint32_t *height)
{
	struct entry_backend_harfbuzz *hb = &entry->harfbuzz;
	cairo_t *cr = entry->cairo[0].cr;
	uint32_t font_size = floor(entry->font_size * PT_TO_DPI);

	/*
	 * Setting up our font has three main steps:
	 *
	 * 1. Load the font face with FreeType.
	 * 2. Create a HarfBuzz font referencing the FreeType font.
	 * 3. Create a Cairo font referencing the FreeType font.
	 *
	 * The simultaneous interaction of Cairo and HarfBuzz with FreeType is
	 * a little finicky, so the order of the last two steps is important.
	 * We use HarfBuzz to set font variation settings (such as weight), if
	 * any. This modifies the underlying FreeType font, so we must create
	 * the Cairo font *after* this point for the changes to take effect.
	 *
	 * This doesn't seem like it should be necessary, as both HarfBuzz and
	 * Cairo reference the same FreeType font, but it is.
	 */

	/* Setup FreeType. */
	log_debug("Creating FreeType library.\n");
	int err;
	err = FT_Init_FreeType(&hb->ft_library);
	if (err) {
		log_error("Error initialising FreeType: %s\n",
				get_ft_error_string(err));
		exit(EXIT_FAILURE);
	}

	log_debug("Loading FreeType font.\n");
	err = FT_New_Face(
			hb->ft_library,
			entry->font_name,
			0,
			&hb->ft_face);
	if (err) {
		log_error("Error loading font: %s\n", get_ft_error_string(err));
		exit(EXIT_FAILURE);
	}

	err = FT_Set_Char_Size(
			hb->ft_face,
			font_size * 64,
			font_size * 64,
			0,
			0);
	if (err) {
		log_error("Error setting font size: %s\n",
				get_ft_error_string(err));
	}

	log_debug("Creating Harfbuzz font.\n");
	hb->hb_font = hb_ft_font_create_referenced(hb->ft_face);

	if (entry->font_variations[0] != 0) {
		log_debug("Parsing font variations.\n");
	}
	char *saveptr = NULL;
	char *variation = strtok_r(entry->font_variations, ",", &saveptr);
	while (variation != NULL && hb->num_variations < N_ELEM(hb->hb_variations)) {
		if (hb_variation_from_string(variation, -1, &hb->hb_variations[hb->num_variations])) {
			hb->num_variations++;
		} else {
			log_error("Failed to parse font variation \"%s\".\n", variation);
		}
		variation = strtok_r(NULL, ",", &saveptr);
	}

	/*
	 * We need to set variations now and update the underlying FreeType
	 * font, as Cairo will then use the FreeType font for drawing.
	 */
	hb_font_set_variations(hb->hb_font, hb->hb_variations, hb->num_variations);
#ifndef NO_HARFBUZZ_FONT_CHANGED
	hb_ft_hb_font_changed(hb->hb_font);
#endif

	if (entry->font_features[0] != 0) {
		log_debug("Parsing font features.\n");
	}
	saveptr = NULL;
	char *feature = strtok_r(entry->font_features, ",", &saveptr);
	while (feature != NULL && hb->num_features < N_ELEM(hb->hb_features)) {
		if (hb_feature_from_string(feature, -1, &hb->hb_features[hb->num_features])) {
			hb->num_features++;
		} else {
			log_error("Failed to parse font feature \"%s\".\n", feature);
		}
		feature = strtok_r(NULL, ",", &saveptr);
	}

	log_debug("Creating Harfbuzz buffer.\n");
	hb->hb_buffer = hb_buffer_create();

	log_debug("Creating Cairo font.\n");
	hb->cairo_face = cairo_ft_font_face_create_for_ft_face(hb->ft_face, 0);

	struct color color = entry->foreground_color;
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
	cairo_set_font_face(cr, hb->cairo_face);
	cairo_set_font_size(cr, font_size);
	cairo_font_options_t *opts = cairo_font_options_create();
	if (hb->disable_hinting) {
		cairo_font_options_set_hint_style(opts, CAIRO_HINT_STYLE_NONE);
	} else {
		cairo_font_options_set_hint_metrics(opts, CAIRO_HINT_METRICS_ON);
	}
	cairo_set_font_options(cr, opts);

	/* We also need to set up the font for our other Cairo context. */
	cairo_set_font_face(entry->cairo[1].cr, hb->cairo_face);
	cairo_set_font_size(entry->cairo[1].cr, font_size);
	cairo_set_font_options(entry->cairo[1].cr, opts);

	cairo_font_options_destroy(opts);
}

void entry_backend_harfbuzz_destroy(struct entry *entry)
{
	hb_buffer_destroy(entry->harfbuzz.hb_buffer);
	hb_font_destroy(entry->harfbuzz.hb_font);
	cairo_font_face_destroy(entry->harfbuzz.cairo_face);
	FT_Done_Face(entry->harfbuzz.ft_face);
	FT_Done_FreeType(entry->harfbuzz.ft_library);
}

void entry_backend_harfbuzz_update(struct entry *entry)
{
	cairo_t *cr = entry->cairo[entry->index].cr;
	cairo_text_extents_t extents;

	cairo_save(cr);

	/* Render the prompt */
	extents = render_text_themed(cr, &entry->harfbuzz, entry->prompt_text, &entry->prompt_theme);

	cairo_translate(cr, extents.x_advance, 0);
	cairo_translate(cr, entry->prompt_padding, 0);

	/* Render the entry text */
	if (entry->input_utf8_length == 0) {
		extents = render_text_themed(cr, &entry->harfbuzz, entry->placeholder_text, &entry->placeholder_theme);
	} else if (entry->hide_input) {
		size_t nchars = entry->input_utf32_length;
		size_t char_size = entry->hidden_character_utf8_length;
		char *buf = xmalloc(1 + nchars * char_size);
		for (size_t i = 0; i < nchars; i++) {
			for (size_t j = 0; j < char_size; j++) {
				buf[i * char_size + j] = entry->hidden_character_utf8[j];
			}
		}
		buf[char_size * nchars] = '\0';

		extents = render_text_themed(cr, &entry->harfbuzz, buf, &entry->input_theme);
		free(buf);
	} else {
		extents = render_text_themed(cr, &entry->harfbuzz, entry->input_utf8, &entry->input_theme);
	}
	extents.x_advance = MAX(extents.x_advance, entry->input_width);

	cairo_font_extents_t font_extents;
	cairo_font_extents(cr, &font_extents);

	uint32_t num_results;
	if (entry->num_results == 0) {
		num_results = entry->results.count;
	} else {
		num_results = MIN(entry->num_results, entry->results.count);
	}
	/* Render our results */
	size_t i;
	for (i = 0; i < num_results; i++) {
		if (entry->horizontal) {
			cairo_translate(cr, extents.x_advance + entry->result_spacing, 0);
		} else {
			cairo_translate(cr, 0, font_extents.height + entry->result_spacing);
		}
		if (entry->num_results == 0) {
			if (size_overflows(entry, 0, 0)) {
				break;
			}
		} else if (i >= entry->num_results) {
			break;
		}


		size_t index = i + entry->first_result;
		/*
		 * We may be on the last page, which could have fewer results
		 * than expected, so check and break if necessary.
		 */
		if (index >= entry->results.count) {
			break;
		}

		const char *result = entry->results.buf[index].string;
		/*
		 * If this isn't the selected result, or it is but we're not
		 * doing any fancy match-highlighting, just print as normal.
		 */
		if (i != entry->selection || (entry->selection_highlight_color.a == 0)) {
			const struct text_theme *theme;
			if (i == entry->selection) {
				theme = &entry->selection_theme;
			} else if (index % 2) {
				theme = &entry->alternate_result_theme;;
			} else {
				theme = &entry->default_result_theme;;
			}

			if (entry->num_results > 0) {
				/*
				 * We're not auto-detecting how many results we
				 * can fit, so just render the text.
				 */
				extents = render_text_themed(cr, &entry->harfbuzz, result, theme);
			} else if (!entry->horizontal) {
				/*
				 * The height of the text doesn't change, so
				 * we don't need to re-measure it each time.
				 */
				if (size_overflows(entry, 0, font_extents.height)) {
					entry->num_results_drawn = i;
					break;
				} else {
					extents = render_text_themed(cr, &entry->harfbuzz, result, theme);
				}
			} else {
				/*
				 * The difficult case: we're auto-detecting how
				 * many results to draw, but we can't know
				 * whether this result will fit without
				 * drawing it! To solve this, draw to a
				 * temporary group, measure that, then copy it
				 * to the main canvas only if it will fit.
				 */
				cairo_push_group(cr);
				extents = render_text_themed(cr, &entry->harfbuzz, result, theme);

				cairo_pattern_t *group = cairo_pop_group(cr);
				if (size_overflows(entry, extents.x_advance, 0)) {
					entry->num_results_drawn = i;
					cairo_pattern_destroy(group);
					break;
				} else {
					cairo_save(cr);
					cairo_set_source(cr, group);
					cairo_paint(cr);
					cairo_restore(cr);
					cairo_pattern_destroy(group);
				}
			}
		} else {
			/*
			 * For match highlighting, there's a bit more to do.
			 *
			 * We need to split the text into prematch, match and
			 * postmatch chunks, and draw each separately.
			 *
			 * However, we only want one background box around them
			 * all (if we're drawing one). To do this, we have to
			 * do the rendering part of render_text_themed()
			 * manually, with the same method of:
			 * - Draw the text and measure it
			 * - Draw the box
			 * - Draw the text again
			 *
			 * N.B. The size_overflows check isn't necessary here,
			 * as it's currently not possible for the selection to
			 * do so.
			 */
			size_t prematch_len;
			size_t postmatch_len;
			char *prematch = xstrdup(result);
			char *match = NULL;
			char *postmatch = NULL;
			if (entry->input_utf8_length > 0 && entry->selection_highlight_color.a != 0) {
				char *match_pos = utf8_strcasestr(prematch, entry->input_utf8);
				if (match_pos != NULL) {
					match = xstrdup(result);
					prematch_len = (match_pos - prematch);
					prematch[prematch_len] = '\0';
					postmatch_len = strlen(result) - prematch_len - entry->input_utf8_length;
					if (postmatch_len > 0) {
						postmatch = xstrdup(result);
					}
					match[entry->input_utf8_length + prematch_len] = '\0';
				}
			}

			for (int pass = 0; pass < 2; pass++) {
				cairo_save(cr);
				struct color color = entry->selection_theme.foreground_color;
				cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);

				cairo_text_extents_t subextents = render_text(cr, &entry->harfbuzz, prematch);
				extents = subextents;

				if (match != NULL) {
					cairo_translate(cr, subextents.x_advance, 0);
					color = entry->selection_highlight_color;
					cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);

					subextents = render_text(cr, &entry->harfbuzz, &match[prematch_len]);

					if (prematch_len == 0) {
						extents = subextents;
					} else {
						/*
						 * This calculation is a little
						 * complex, but it's basically:
						 *
						 * (distance from leftmost pixel of
						 * prematch to logical end of prematch)
						 * 
						 * +
						 *
						 * (distance from logical start of match
						 * to rightmost pixel of match).
						 */
						extents.width = extents.x_advance
							- extents.x_bearing
							+ subextents.x_bearing
							+ subextents.width;
						extents.x_advance += subextents.x_advance;
					}
				}

				if (postmatch != NULL) {
					cairo_translate(cr, subextents.x_advance, 0);
					color = entry->selection_theme.foreground_color;
					cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
					subextents = render_text(
							cr,
							&entry->harfbuzz,
							&postmatch[entry->input_utf8_length + prematch_len]);

					extents.width = extents.x_advance
						- extents.x_bearing
						+ subextents.x_bearing
						+ subextents.width;
					extents.x_advance += subextents.x_advance;
				}

				cairo_restore(cr);

				if (entry->selection_theme.background_color.a == 0) {
					/* No background box, we're done. */
					break;
				} else if (pass == 0) {
					/* 
					 * First pass, paint over the text with
					 * our background box.
					 */
					struct directional padding = entry->selection_theme.padding;
					cairo_save(cr);
					color = entry->selection_theme.background_color;
					cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
					cairo_translate(
							cr,
							floor(-padding.left + extents.x_bearing),
							-padding.top);
					rounded_rectangle(
							cr,
							ceil(extents.width + padding.left + padding.right),
							ceil(font_extents.height + padding.top + padding.bottom),
							entry->selection_theme.background_corner_radius
							);
					cairo_fill(cr);
					cairo_restore(cr);
				}
			}

			free(prematch);
			if (match != NULL) {
				free(match);
			}
			if (postmatch != NULL) {
				free(postmatch);
			}
		}
	}
	entry->num_results_drawn = i;
	log_debug("Drew %zu results.\n", i);

	cairo_restore(cr);
}
