/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2015-2016 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <glib/gi18n.h>

#include "gs-content-rating.h"

const gchar *
gs_content_rating_system_to_str (GsContentRatingSystem system)
{
	if (system == GS_CONTENT_RATING_SYSTEM_INCAA)
		return "INCAA";
	if (system == GS_CONTENT_RATING_SYSTEM_ACB)
		return "ACB";
	if (system == GS_CONTENT_RATING_SYSTEM_DJCTQ)
		return "DJCTQ";
	if (system == GS_CONTENT_RATING_SYSTEM_GSRR)
		return "GSRR";
	if (system == GS_CONTENT_RATING_SYSTEM_PEGI)
		return "PEGI";
	if (system == GS_CONTENT_RATING_SYSTEM_KAVI)
		return "KAVI";
	if (system == GS_CONTENT_RATING_SYSTEM_USK)
		return "USK";
	if (system == GS_CONTENT_RATING_SYSTEM_ESRA)
		return "ESRA";
	if (system == GS_CONTENT_RATING_SYSTEM_CERO)
		return "CERO";
	if (system == GS_CONTENT_RATING_SYSTEM_OFLCNZ)
		return "OFLCNZ";
	if (system == GS_CONTENT_RATING_SYSTEM_RUSSIA)
		return "RUSSIA";
	if (system == GS_CONTENT_RATING_SYSTEM_MDA)
		return "MDA";
	if (system == GS_CONTENT_RATING_SYSTEM_GRAC)
		return "GRAC";
	if (system == GS_CONTENT_RATING_SYSTEM_ESRB)
		return "ESRB";
	if (system == GS_CONTENT_RATING_SYSTEM_IARC)
		return "IARC";
	return NULL;
}

const gchar *
gs_content_rating_key_value_to_str (const gchar *id, EpcAppFilterOarsValue value)
{
	guint i;
	const struct {
		const gchar		*id;
		EpcAppFilterOarsValue	 value;
		const gchar		*desc;
	} tab[] =  {
	{ "violence-cartoon",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No cartoon violence") },
	{ "violence-cartoon",	EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("Cartoon characters in unsafe situations") },
	{ "violence-cartoon",	EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Cartoon characters in aggressive conflict") },
	{ "violence-cartoon",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Graphic violence involving cartoon characters") },
	{ "violence-fantasy",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No fantasy violence") },
	{ "violence-fantasy",	EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("Characters in unsafe situations easily distinguishable from reality") },
	{ "violence-fantasy",	EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Characters in aggressive conflict easily distinguishable from reality") },
	{ "violence-fantasy",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Graphic violence easily distinguishable from reality") },
	{ "violence-realistic",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No realistic violence") },
	{ "violence-realistic",	EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("Mildly realistic characters in unsafe situations") },
	{ "violence-realistic",	EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Depictions of realistic characters in aggressive conflict") },
	{ "violence-realistic",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Graphic violence involving realistic characters") },
	{ "violence-bloodshed",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No bloodshed") },
	{ "violence-bloodshed",	EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("Unrealistic bloodshed") },
	{ "violence-bloodshed",	EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Realistic bloodshed") },
	{ "violence-bloodshed",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Depictions of bloodshed and the mutilation of body parts") },
	{ "violence-sexual",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No sexual violence") },
	{ "violence-sexual",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Rape or other violent sexual behavior") },
	{ "drugs-alcohol",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No references to alcohol") },
	{ "drugs-alcohol",	EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("References to alcoholic beverages") },
	{ "drugs-alcohol",	EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Use of alcoholic beverages") },
	{ "drugs-narcotics",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No references to illicit drugs") },
	{ "drugs-narcotics",	EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("References to illicit drugs") },
	{ "drugs-narcotics",	EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Use of illicit drugs") },
	{ "drugs-tobacco",	EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("References to tobacco products") },
	{ "drugs-tobacco",	EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Use of tobacco products") },
	{ "sex-nudity",		EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No nudity of any sort") },
	{ "sex-nudity",		EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("Brief artistic nudity") },
	{ "sex-nudity",		EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Prolonged nudity") },
	{ "sex-themes",		EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No references or depictions of sexual nature") },
	{ "sex-themes",		EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("Provocative references or depictions") },
	{ "sex-themes",		EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Sexual references or depictions") },
	{ "sex-themes",		EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Graphic sexual behavior") },
	{ "language-profanity",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No profanity of any kind") },
	{ "language-profanity",	EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("Mild or infrequent use of profanity") },
	{ "language-profanity",	EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Moderate use of profanity") },
	{ "language-profanity",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Strong or frequent use of profanity") },
	{ "language-humor",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No inappropriate humor") },
	{ "language-humor",	EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("Slapstick humor") },
	{ "language-humor",	EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Vulgar or bathroom humor") },
	{ "language-humor",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Mature or sexual humor") },
	{ "language-discrimination", EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No discriminatory language of any kind") },
	{ "language-discrimination", EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("Negativity towards a specific group of people") },
	{ "language-discrimination", EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Discrimination designed to cause emotional harm") },
	{ "language-discrimination", EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Explicit discrimination based on gender, sexuality, race or religion") },
	{ "money-advertising", EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No advertising of any kind") },
	{ "money-advertising", EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("Product placement") },
	{ "money-advertising", EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Explicit references to specific brands or trademarked products") },
	{ "money-advertising", EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Users are encouraged to purchase specific real-world items") },
	{ "money-gambling",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No gambling of any kind") },
	{ "money-gambling",	EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("Gambling on random events using tokens or credits") },
	{ "money-gambling",	EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Gambling using “play” money") },
	{ "money-gambling",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Gambling using real money") },
	{ "money-purchasing",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No ability to spend money") },
	{ "money-purchasing",	EPC_APP_FILTER_OARS_VALUE_MILD,		/* v1.1 */
	/* TRANSLATORS: content rating description */
	_("Users are encouraged to donate real money") },
	{ "money-purchasing",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Ability to spend real money in-game") },
	{ "social-chat",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No way to chat with other users") },
	{ "social-chat",	EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("User-to-user game interactions without chat functionality") },
	{ "social-chat",	EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Moderated chat functionality between users") },
	{ "social-chat",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Uncontrolled chat functionality between users") },
	{ "social-audio",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No way to talk with other users") },
	{ "social-audio",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Uncontrolled audio or video chat functionality between users") },
	{ "social-contacts",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No sharing of social network usernames or email addresses") },
	{ "social-contacts",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Sharing social network usernames or email addresses") },
	{ "social-info",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No sharing of user information with 3rd parties") },
	{ "social-info",	EPC_APP_FILTER_OARS_VALUE_MILD,		/* v1.1 */
	/* TRANSLATORS: content rating description */
	_("Checking for the latest application version") },
	{ "social-info",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	/* v1.1 */
	/* TRANSLATORS: content rating description */
	_("Sharing diagnostic data that does not let others identify the user") },
	{ "social-info",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Sharing information that lets others identify the user") },
	{ "social-location",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No sharing of physical location to other users") },
	{ "social-location",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Sharing physical location to other users") },

	/* v1.1 */
	{ "sex-homosexuality",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No references to homosexuality") },
	{ "sex-homosexuality",	EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("Indirect references to homosexuality") },
	{ "sex-homosexuality",	EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Kissing between people of the same gender") },
	{ "sex-homosexuality",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Graphic sexual behavior between people of the same gender") },
	{ "sex-prostitution",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No references to prostitution") },
	{ "sex-prostitution",	EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("Indirect references to prostitution") },
	{ "sex-prostitution",	EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Direct references to prostitution") },
	{ "sex-prostitution",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Graphic depictions of the act of prostitution") },
	{ "sex-adultery",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No references to adultery") },
	{ "sex-adultery",	EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("Indirect references to adultery") },
	{ "sex-adultery",	EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Direct references to adultery") },
	{ "sex-adultery",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Graphic depictions of the act of adultery") },
	{ "sex-appearance",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No sexualized characters") },
	{ "sex-appearance",	EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Scantily clad human characters") },
	{ "sex-appearance",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Overtly sexualized human characters") },
	{ "violence-worship",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No references to desecration") },
	{ "violence-worship",	EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("Depictions or references to historical desecration") },
	{ "violence-worship",	EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Depictions of modern-day human desecration") },
	{ "violence-worship",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Graphic depictions of modern-day desecration") },
	{ "violence-desecration", EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No visible dead human remains") },
	{ "violence-desecration", EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("Visible dead human remains") },
	{ "violence-desecration", EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Dead human remains that are exposed to the elements") },
	{ "violence-desecration", EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Graphic depictions of desecration of human bodies") },
	{ "violence-slavery",	EPC_APP_FILTER_OARS_VALUE_NONE,
	/* TRANSLATORS: content rating description */
	_("No references to slavery") },
	{ "violence-slavery",	EPC_APP_FILTER_OARS_VALUE_MILD,
	/* TRANSLATORS: content rating description */
	_("Depictions or references to historical slavery") },
	{ "violence-slavery",	EPC_APP_FILTER_OARS_VALUE_MODERATE,
	/* TRANSLATORS: content rating description */
	_("Depictions of modern-day slavery") },
	{ "violence-slavery",	EPC_APP_FILTER_OARS_VALUE_INTENSE,
	/* TRANSLATORS: content rating description */
	_("Graphic depictions of modern-day slavery") },
	{ NULL, 0, NULL } };
	for (i = 0; tab[i].id != NULL; i++) {
		if (g_strcmp0 (tab[i].id, id) == 0 && tab[i].value == value)
			return tab[i].desc;
	}
	return NULL;
}

/* data obtained from https://en.wikipedia.org/wiki/Video_game_rating_system */
const gchar *
gs_utils_content_rating_age_to_str (GsContentRatingSystem system, guint age)
{
	if (system == GS_CONTENT_RATING_SYSTEM_INCAA) {
		if (age >= 18)
			return "+18";
		if (age >= 13)
			return "+13";
		return "ATP";
	}
	if (system == GS_CONTENT_RATING_SYSTEM_ACB) {
		if (age >= 18)
			return "R18+";
		if (age >= 15)
			return "MA15+";
		return "PG";
	}
	if (system == GS_CONTENT_RATING_SYSTEM_DJCTQ) {
		if (age >= 18)
			return "18";
		if (age >= 16)
			return "16";
		if (age >= 14)
			return "14";
		if (age >= 12)
			return "12";
		if (age >= 10)
			return "10";
		return "L";
	}
	if (system == GS_CONTENT_RATING_SYSTEM_GSRR) {
		if (age >= 18)
			return "限制";
		if (age >= 15)
			return "輔15";
		if (age >= 12)
			return "輔12";
		if (age >= 6)
			return "保護";
		return "普通";
	}
	if (system == GS_CONTENT_RATING_SYSTEM_PEGI) {
		if (age >= 18)
			return "18";
		if (age >= 16)
			return "16";
		if (age >= 12)
			return "12";
		if (age >= 7)
			return "7";
		if (age >= 3)
			return "3";
		return NULL;
	}
	if (system == GS_CONTENT_RATING_SYSTEM_KAVI) {
		if (age >= 18)
			return "18+";
		if (age >= 16)
			return "16+";
		if (age >= 12)
			return "12+";
		if (age >= 7)
			return "7+";
		if (age >= 3)
			return "3+";
		return NULL;
	}
	if (system == GS_CONTENT_RATING_SYSTEM_USK) {
		if (age >= 18)
			return "18";
		if (age >= 16)
			return "16";
		if (age >= 12)
			return "12";
		if (age >= 6)
			return "6";
		return "0";
	}
	if (system == GS_CONTENT_RATING_SYSTEM_ESRA) {
		if (age >= 25)
			return "+25";
		if (age >= 18)
			return "+18";
		if (age >= 12)
			return "+12";
		if (age >= 7)
			return "+7";
		if (age >= 3)
			return "+3";
		return NULL;
	}
	if (system == GS_CONTENT_RATING_SYSTEM_CERO) {
		if (age >= 18)
			return "Z";
		if (age >= 17)
			return "D";
		if (age >= 15)
			return "C";
		if (age >= 12)
			return "B";
		return "A";
	}
	if (system == GS_CONTENT_RATING_SYSTEM_OFLCNZ) {
		if (age >= 18)
			return "R18";
		if (age >= 16)
			return "R16";
		if (age >= 15)
			return "R15";
		if (age >= 13)
			return "R13";
		return "G";
	}
	if (system == GS_CONTENT_RATING_SYSTEM_RUSSIA) {
		if (age >= 18)
			return "18+";
		if (age >= 16)
			return "16+";
		if (age >= 12)
			return "12+";
		if (age >= 6)
			return "6+";
		return "0+";
	}
	if (system == GS_CONTENT_RATING_SYSTEM_MDA) {
		if (age >= 18)
			return "M18";
		if (age >= 16)
			return "ADV";
		return "General";
	}
	if (system == GS_CONTENT_RATING_SYSTEM_GRAC) {
		if (age >= 18)
			return "18";
		if (age >= 15)
			return "15";
		if (age >= 12)
			return "12";
		return "ALL";
	}
	if (system == GS_CONTENT_RATING_SYSTEM_ESRB) {
		if (age >= 18)
			return "Adults Only";
		if (age >= 17)
			return "Mature";
		if (age >= 13)
			return "Teen";
		if (age >= 10)
			return "Everyone 10+";
		if (age >= 6)
			return "Everyone";
		return "Early Childhood";
	}
	/* IARC = everything else */
	if (age >= 18)
		return "18+";
	if (age >= 16)
		return "16+";
	if (age >= 12)
		return "12+";
	if (age >= 7)
		return "7+";
	if (age >= 3)
		return "3+";
	return NULL;
}

/* data obtained from https://en.wikipedia.org/wiki/Video_game_rating_system */
GsContentRatingSystem
gs_utils_content_rating_system_from_locale (const gchar *locale)
{
	g_auto(GStrv) split = g_strsplit (locale, "_", -1);

	/* Argentina */
	if (g_strcmp0 (split[0], "ar") == 0)
		return GS_CONTENT_RATING_SYSTEM_INCAA;

	/* Australia */
	if (g_strcmp0 (split[0], "au") == 0)
		return GS_CONTENT_RATING_SYSTEM_ACB;

	/* Brazil */
	if (g_strcmp0 (locale, "pt_BR") == 0)
		return GS_CONTENT_RATING_SYSTEM_DJCTQ;

	/* Taiwan */
	if (g_strcmp0 (locale, "zh_TW") == 0)
		return GS_CONTENT_RATING_SYSTEM_GSRR;

	/* Europe (but not Finland or Germany), India, Israel,
	 * Pakistan, Quebec, South Africa */
	if (g_strcmp0 (locale, "en_GB") == 0 ||
	    g_strcmp0 (split[0], "gb") == 0 ||
	    g_strcmp0 (split[0], "al") == 0 ||
	    g_strcmp0 (split[0], "ad") == 0 ||
	    g_strcmp0 (split[0], "am") == 0 ||
	    g_strcmp0 (split[0], "at") == 0 ||
	    g_strcmp0 (split[0], "az") == 0 ||
	    g_strcmp0 (split[0], "by") == 0 ||
	    g_strcmp0 (split[0], "be") == 0 ||
	    g_strcmp0 (split[0], "ba") == 0 ||
	    g_strcmp0 (split[0], "bg") == 0 ||
	    g_strcmp0 (split[0], "hr") == 0 ||
	    g_strcmp0 (split[0], "cy") == 0 ||
	    g_strcmp0 (split[0], "cz") == 0 ||
	    g_strcmp0 (split[0], "dk") == 0 ||
	    g_strcmp0 (split[0], "ee") == 0 ||
	    g_strcmp0 (split[0], "fr") == 0 ||
	    g_strcmp0 (split[0], "ge") == 0 ||
	    g_strcmp0 (split[0], "gr") == 0 ||
	    g_strcmp0 (split[0], "hu") == 0 ||
	    g_strcmp0 (split[0], "is") == 0 ||
	    g_strcmp0 (split[0], "it") == 0 ||
	    g_strcmp0 (split[0], "kz") == 0 ||
	    g_strcmp0 (split[0], "xk") == 0 ||
	    g_strcmp0 (split[0], "lv") == 0 ||
	    g_strcmp0 (split[0], "fl") == 0 ||
	    g_strcmp0 (split[0], "lu") == 0 ||
	    g_strcmp0 (split[0], "lt") == 0 ||
	    g_strcmp0 (split[0], "mk") == 0 ||
	    g_strcmp0 (split[0], "mt") == 0 ||
	    g_strcmp0 (split[0], "md") == 0 ||
	    g_strcmp0 (split[0], "mc") == 0 ||
	    g_strcmp0 (split[0], "me") == 0 ||
	    g_strcmp0 (split[0], "nl") == 0 ||
	    g_strcmp0 (split[0], "no") == 0 ||
	    g_strcmp0 (split[0], "pl") == 0 ||
	    g_strcmp0 (split[0], "pt") == 0 ||
	    g_strcmp0 (split[0], "ro") == 0 ||
	    g_strcmp0 (split[0], "sm") == 0 ||
	    g_strcmp0 (split[0], "rs") == 0 ||
	    g_strcmp0 (split[0], "sk") == 0 ||
	    g_strcmp0 (split[0], "si") == 0 ||
	    g_strcmp0 (split[0], "es") == 0 ||
	    g_strcmp0 (split[0], "se") == 0 ||
	    g_strcmp0 (split[0], "ch") == 0 ||
	    g_strcmp0 (split[0], "tr") == 0 ||
	    g_strcmp0 (split[0], "ua") == 0 ||
	    g_strcmp0 (split[0], "va") == 0 ||
	    g_strcmp0 (split[0], "in") == 0 ||
	    g_strcmp0 (split[0], "il") == 0 ||
	    g_strcmp0 (split[0], "pk") == 0 ||
	    g_strcmp0 (split[0], "za") == 0)
		return GS_CONTENT_RATING_SYSTEM_PEGI;

	/* Finland */
	if (g_strcmp0 (split[0], "fi") == 0)
		return GS_CONTENT_RATING_SYSTEM_KAVI;

	/* Germany */
	if (g_strcmp0 (split[0], "de") == 0)
		return GS_CONTENT_RATING_SYSTEM_USK;

	/* Iran */
	if (g_strcmp0 (split[0], "ir") == 0)
		return GS_CONTENT_RATING_SYSTEM_ESRA;

	/* Japan */
	if (g_strcmp0 (split[0], "jp") == 0)
		return GS_CONTENT_RATING_SYSTEM_CERO;

	/* New Zealand */
	if (g_strcmp0 (split[0], "nz") == 0)
		return GS_CONTENT_RATING_SYSTEM_OFLCNZ;

	/* Russia: Content rating law */
	if (g_strcmp0 (split[0], "ru") == 0)
		return GS_CONTENT_RATING_SYSTEM_RUSSIA;

	/* Singapore */
	if (g_strcmp0 (split[0], "sg") == 0)
		return GS_CONTENT_RATING_SYSTEM_MDA;

	/* South Korea */
	if (g_strcmp0 (split[0], "kr") == 0)
		return GS_CONTENT_RATING_SYSTEM_GRAC;

	/* USA, Canada, Mexico */
	if (g_strcmp0 (locale, "en_US") == 0 ||
	    g_strcmp0 (split[0], "us") == 0 ||
	    g_strcmp0 (split[0], "ca") == 0 ||
	    g_strcmp0 (split[0], "mx") == 0)
		return GS_CONTENT_RATING_SYSTEM_ESRB;

	/* everything else is IARC */
	return GS_CONTENT_RATING_SYSTEM_IARC;
}

static const gchar *content_rating_strings[GS_CONTENT_RATING_SYSTEM_LAST][7] = {
	{ "3+", "7+", "12+", "16+", "18+", NULL }, /* GS_CONTENT_RATING_SYSTEM_UNKNOWN */
	{ "ATP", "+13", "+18", NULL }, /* GS_CONTENT_RATING_SYSTEM_INCAA */
	{ "PG", "MA15+", "R18+", NULL }, /* GS_CONTENT_RATING_SYSTEM_ACB */
	{ "L", "10", "12", "14", "16", "18", NULL }, /* GS_CONTENT_RATING_SYSTEM_DJCTQ */
	{ "普通", "保護", "輔12", "輔15", "限制", NULL }, /* GS_CONTENT_RATING_SYSTEM_GSRR */
	{ "3", "7", "12", "16", "18", NULL }, /* GS_CONTENT_RATING_SYSTEM_PEGI */
	{ "3+", "7+", "12+", "16+", "18+", NULL }, /* GS_CONTENT_RATING_SYSTEM_KAVI */
	{ "0", "6", "12", "16", "18", NULL}, /* GS_CONTENT_RATING_SYSTEM_USK */
	{ "+3", "+7", "+12", "+18", "+25", NULL }, /* GS_CONTENT_RATING_SYSTEM_ESRA */
	{ "A", "B", "C", "D", "Z", NULL }, /* GS_CONTENT_RATING_SYSTEM_CERO */
	{ "G", "R13", "R15", "R16", "R18", NULL }, /* GS_CONTENT_RATING_SYSTEM_OFLCNZ */
	{ "0+", "6+", "12+", "16+", "18+", NULL }, /* GS_CONTENT_RATING_SYSTEM_RUSSIA */
	{ "General", "ADV", "M18", NULL }, /* GS_CONTENT_RATING_SYSTEM_MDA */
	{ "ALL", "12", "15", "18", NULL }, /* GS_CONTENT_RATING_SYSTEM_GRAC */
	{ "Early Childhood", "Everyone", "Everyone 10+", "Teen", "Mature", "Adults Only", NULL }, /* GS_CONTENT_RATING_SYSTEM_ESRB */
	{ "3+", "7+", "12+", "16+", "18+", NULL }, /* GS_CONTENT_RATING_SYSTEM_IARC */
};

const gchar * const *
gs_utils_content_rating_get_values (GsContentRatingSystem system)
{
	g_assert (system < GS_CONTENT_RATING_SYSTEM_LAST);
	return content_rating_strings[system];
}

static guint content_rating_ages[GS_CONTENT_RATING_SYSTEM_LAST][7] = {
	{ 3, 7, 12, 16, 18 }, /* GS_CONTENT_RATING_SYSTEM_UNKNOWN */
	{ 0, 13, 18 }, /* GS_CONTENT_RATING_SYSTEM_INCAA */
	{ 0, 15, 18 }, /* GS_CONTENT_RATING_SYSTEM_ACB */
	{ 0, 10, 12, 14, 16, 18 }, /* GS_CONTENT_RATING_SYSTEM_DJCTQ */
	{ 0, 6, 12, 15, 18 }, /* GS_CONTENT_RATING_SYSTEM_GSRR */
	{ 3, 7, 12, 16, 18 }, /* GS_CONTENT_RATING_SYSTEM_PEGI */
	{ 3, 7, 12, 16, 18 }, /* GS_CONTENT_RATING_SYSTEM_KAVI */
	{ 0, 6, 12, 16, 18 }, /* GS_CONTENT_RATING_SYSTEM_USK */
	{ 3, 7, 12, 18, 25 }, /* GS_CONTENT_RATING_SYSTEM_ESRA */
	{ 0, 12, 15, 17, 18 }, /* GS_CONTENT_RATING_SYSTEM_CERO */
	{ 0, 13, 15, 16, 18 }, /* GS_CONTENT_RATING_SYSTEM_OFLCNZ */
	{ 0, 6, 12, 16, 18 }, /* GS_CONTENT_RATING_SYSTEM_RUSSIA */
	{ 0, 16, 18 }, /* GS_CONTENT_RATING_SYSTEM_MDA */
	{ 0, 12, 15, 18 }, /* GS_CONTENT_RATING_SYSTEM_GRAC */
	{ 0, 6, 10, 13, 17, 18 }, /* GS_CONTENT_RATING_SYSTEM_ESRB */
	{ 3, 7, 12, 16, 18 }, /* GS_CONTENT_RATING_SYSTEM_IARC */
};

const guint *
gs_utils_content_rating_get_ages (GsContentRatingSystem system)
{
	g_assert (system < GS_CONTENT_RATING_SYSTEM_LAST);
	return content_rating_ages[system];
}

const struct {
	const gchar		*id;
	EpcAppFilterOarsValue	 value;
	guint			 csm_age;
} id_to_csm_age[] =  {
/* v1.0 */
{ "violence-cartoon",	EPC_APP_FILTER_OARS_VALUE_NONE,		0 },
{ "violence-cartoon",	EPC_APP_FILTER_OARS_VALUE_MILD,		3 },
{ "violence-cartoon",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	4 },
{ "violence-cartoon",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	6 },
{ "violence-fantasy",	EPC_APP_FILTER_OARS_VALUE_NONE,		0 },
{ "violence-fantasy",	EPC_APP_FILTER_OARS_VALUE_MILD,		3 },
{ "violence-fantasy",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	7 },
{ "violence-fantasy",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	8 },
{ "violence-realistic",	EPC_APP_FILTER_OARS_VALUE_NONE,		0 },
{ "violence-realistic",	EPC_APP_FILTER_OARS_VALUE_MILD,		4 },
{ "violence-realistic",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	9 },
{ "violence-realistic",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	14 },
{ "violence-bloodshed",	EPC_APP_FILTER_OARS_VALUE_NONE,		0 },
{ "violence-bloodshed",	EPC_APP_FILTER_OARS_VALUE_MILD,		9 },
{ "violence-bloodshed",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	11 },
{ "violence-bloodshed",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	18 },
{ "violence-sexual",	EPC_APP_FILTER_OARS_VALUE_NONE,		0 },
{ "violence-sexual",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	18 },
{ "drugs-alcohol",	EPC_APP_FILTER_OARS_VALUE_NONE,		0 },
{ "drugs-alcohol",	EPC_APP_FILTER_OARS_VALUE_MILD,		11 },
{ "drugs-alcohol",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	13 },
{ "drugs-narcotics",	EPC_APP_FILTER_OARS_VALUE_NONE,		0 },
{ "drugs-narcotics",	EPC_APP_FILTER_OARS_VALUE_MILD,		12 },
{ "drugs-narcotics",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	14 },
{ "drugs-tobacco",	EPC_APP_FILTER_OARS_VALUE_NONE,		0 },
{ "drugs-tobacco",	EPC_APP_FILTER_OARS_VALUE_MILD,		10 },
{ "drugs-tobacco",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	13 },
{ "sex-nudity",		EPC_APP_FILTER_OARS_VALUE_NONE,		0  },
{ "sex-nudity",		EPC_APP_FILTER_OARS_VALUE_MILD,		12 },
{ "sex-nudity",		EPC_APP_FILTER_OARS_VALUE_MODERATE,	14 },
{ "sex-themes",		EPC_APP_FILTER_OARS_VALUE_NONE,		0  },
{ "sex-themes",		EPC_APP_FILTER_OARS_VALUE_MILD,		13 },
{ "sex-themes",		EPC_APP_FILTER_OARS_VALUE_MODERATE,	14 },
{ "sex-themes",		EPC_APP_FILTER_OARS_VALUE_INTENSE,	15 },
{ "language-profanity",	EPC_APP_FILTER_OARS_VALUE_NONE,		0  },
{ "language-profanity",	EPC_APP_FILTER_OARS_VALUE_MILD,		8  },
{ "language-profanity",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	11 },
{ "language-profanity",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	14 },
{ "language-humor",	EPC_APP_FILTER_OARS_VALUE_NONE,		0  },
{ "language-humor",	EPC_APP_FILTER_OARS_VALUE_MILD,		3  },
{ "language-humor",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	8  },
{ "language-humor",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	14 },
{ "language-discrimination", EPC_APP_FILTER_OARS_VALUE_NONE,	0  },
{ "language-discrimination", EPC_APP_FILTER_OARS_VALUE_MILD,	9  },
{ "language-discrimination", EPC_APP_FILTER_OARS_VALUE_MODERATE,10 },
{ "language-discrimination", EPC_APP_FILTER_OARS_VALUE_INTENSE,	11 },
{ "money-advertising",	EPC_APP_FILTER_OARS_VALUE_NONE,		0  },
{ "money-advertising",	EPC_APP_FILTER_OARS_VALUE_MILD,		7  },
{ "money-advertising",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	8  },
{ "money-advertising",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	10 },
{ "money-gambling",	EPC_APP_FILTER_OARS_VALUE_NONE,		0  },
{ "money-gambling",	EPC_APP_FILTER_OARS_VALUE_MILD,		7  },
{ "money-gambling",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	10 },
{ "money-gambling",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	18 },
{ "money-purchasing",	EPC_APP_FILTER_OARS_VALUE_NONE,		0  },
{ "money-purchasing",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	15 },
{ "social-chat",	EPC_APP_FILTER_OARS_VALUE_NONE,		0  },
{ "social-chat",	EPC_APP_FILTER_OARS_VALUE_MILD,		4  },
{ "social-chat",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	10 },
{ "social-chat",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	13 },
{ "social-audio",	EPC_APP_FILTER_OARS_VALUE_NONE,		0  },
{ "social-audio",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	15 },
{ "social-contacts",	EPC_APP_FILTER_OARS_VALUE_NONE,		0  },
{ "social-contacts",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	12 },
{ "social-info",	EPC_APP_FILTER_OARS_VALUE_NONE,		0  },
{ "social-info",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	13 },
{ "social-location",	EPC_APP_FILTER_OARS_VALUE_NONE,		0  },
{ "social-location",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	13 },
/* v1.1 additions */
{ "social-info",	EPC_APP_FILTER_OARS_VALUE_MILD,		0  },
{ "social-info",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	13 },
{ "money-purchasing",	EPC_APP_FILTER_OARS_VALUE_MILD,		12 },
{ "social-chat",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	14 },
{ "sex-homosexuality",	EPC_APP_FILTER_OARS_VALUE_NONE,		0  },
{ "sex-homosexuality",	EPC_APP_FILTER_OARS_VALUE_MILD,		10 },
{ "sex-homosexuality",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	13 },
{ "sex-homosexuality",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	18 },
{ "sex-prostitution",	EPC_APP_FILTER_OARS_VALUE_NONE,		0  },
{ "sex-prostitution",	EPC_APP_FILTER_OARS_VALUE_MILD,		12 },
{ "sex-prostitution",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	14 },
{ "sex-prostitution",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	18 },
{ "sex-adultery",	EPC_APP_FILTER_OARS_VALUE_NONE,		0  },
{ "sex-adultery",	EPC_APP_FILTER_OARS_VALUE_MILD,		8  },
{ "sex-adultery",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	10 },
{ "sex-adultery",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	18 },
{ "sex-appearance",	EPC_APP_FILTER_OARS_VALUE_NONE,		0  },
{ "sex-appearance",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	10 },
{ "sex-appearance",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	15 },
{ "violence-worship",	EPC_APP_FILTER_OARS_VALUE_NONE,		0  },
{ "violence-worship",	EPC_APP_FILTER_OARS_VALUE_MILD,		13 },
{ "violence-worship",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	15 },
{ "violence-worship",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	18 },
{ "violence-desecration", EPC_APP_FILTER_OARS_VALUE_NONE,	0  },
{ "violence-desecration", EPC_APP_FILTER_OARS_VALUE_MILD,	13 },
{ "violence-desecration", EPC_APP_FILTER_OARS_VALUE_MODERATE,	15 },
{ "violence-desecration", EPC_APP_FILTER_OARS_VALUE_INTENSE,	18 },
{ "violence-slavery",	EPC_APP_FILTER_OARS_VALUE_NONE,		0  },
{ "violence-slavery",	EPC_APP_FILTER_OARS_VALUE_MILD,		13 },
{ "violence-slavery",	EPC_APP_FILTER_OARS_VALUE_MODERATE,	15 },
{ "violence-slavery",	EPC_APP_FILTER_OARS_VALUE_INTENSE,	18 },
{ NULL, 0, 0 } };

/**
 * as_content_rating_id_value_to_csm_age:
 * @id: the subsection ID e.g. "violence-cartoon"
 * @value: the #AsContentRatingValue, e.g. %EPC_APP_FILTER_OARS_VALUE_INTENSE
 *
 * Gets the Common Sense Media approved age for a specific rating level.
 *
 * Returns: The age in years, or 0 for no details.
 *
 * Since: 0.5.12
 **/
guint
as_content_rating_id_value_to_csm_age (const gchar *id, EpcAppFilterOarsValue value)
{
	guint i;
	for (i = 0; id_to_csm_age[i].id != NULL; i++) {
		if (value == id_to_csm_age[i].value &&
		    g_strcmp0 (id, id_to_csm_age[i].id) == 0)
			return id_to_csm_age[i].csm_age;
	}
	return 0;
}

/**
 * as_content_rating_id_csm_age_to_value:
 * @id: the subsection ID e.g. "violence-cartoon"
 * @age: the age
 *
 * Gets the #EpcAppFilterOarsValue for a given age.
 *
 * Returns: the #EpcAppFilterOarsValue
 **/
EpcAppFilterOarsValue
as_content_rating_id_csm_age_to_value (const gchar *id, guint age)
{
	EpcAppFilterOarsValue value;
	guint i;

	value = EPC_APP_FILTER_OARS_VALUE_UNKNOWN;

	for (i = 0; id_to_csm_age[i].id != NULL; i++) {
		if (age >= id_to_csm_age[i].csm_age &&
		    g_strcmp0 (id, id_to_csm_age[i].id) == 0)
			value = MAX (value, id_to_csm_age[i].value);
	}
	return value;
}
