// SPDX-License-Identifier: BSD-3-Clause

#include "sdp_pjsip.h"

#include <pjlib.h>
#include <pjmedia/errno.h>

#include <stdio.h>
#include <string.h>

enum {
	SIPPYMODEM_SDP_PJSIP_ATTRIBUTE_VALUE_MAX = 64,
	SIPPYMODEM_SDP_PJSIP_NAME_MAX = 32,
};

static bool pj_text_equals(const pj_str_t *text, const char *expected)
{
	size_t expected_size;

	if (text == NULL || text->ptr == NULL || expected == NULL)
		return false;
	expected_size = strlen(expected);
	return text->slen == (pj_ssize_t)expected_size &&
	       memcmp(text->ptr, expected, expected_size) == 0;
}

static bool attribute_is_media_owned(const pjmedia_sdp_attr *attribute)
{
	char name[SIPPYMODEM_SDP_PJSIP_NAME_MAX];
	size_t size;

	if (attribute == NULL || attribute->name.ptr == NULL ||
	    attribute->name.slen <= 0)
		return false;
	size = (size_t)attribute->name.slen;
	if (size >= sizeof(name))
		return false;
	memcpy(name, attribute->name.ptr, size);
	name[size] = '\0';
	return sippymodem_sdp_attribute_is_media_owned(name);
}

static void remove_media_owned_attributes(pjmedia_sdp_media *media)
{
	unsigned int kept = 0;

	for (unsigned int index = 0; index < media->attr_count; ++index) {
		if (!attribute_is_media_owned(media->attr[index]))
			media->attr[kept++] = media->attr[index];
	}
	media->attr_count = kept;
}

static pj_status_t preflight_attributes(
	const pjmedia_sdp_media *media,
	const struct sippymodem_sdp_audio_description *description)
{
	char value[SIPPYMODEM_SDP_PJSIP_ATTRIBUTE_VALUE_MAX];
	unsigned int retained = 0;
	unsigned int added = 0;

	if (media->attr_count > PJMEDIA_MAX_SDP_ATTR)
		return PJ_EINVAL;
	for (unsigned int index = 0; index < media->attr_count; ++index) {
		if (!attribute_is_media_owned(media->attr[index]))
			retained++;
	}
	for (enum sippymodem_sdp_audio_attribute kind =
		     SIPPYMODEM_SDP_AUDIO_RTPMAP;
	     kind < SIPPYMODEM_SDP_AUDIO_ATTRIBUTE_COUNT; ++kind) {
		enum sippymodem_sdp_result result =
			sippymodem_sdp_audio_format_attribute(
				description, kind, value, sizeof(value));

		if (result == SIPPYMODEM_SDP_OK)
			added++;
		else if (result != SIPPYMODEM_SDP_NOT_PRESENT)
			return PJ_EINVAL;
	}
	return retained <= PJMEDIA_MAX_SDP_ATTR - added ? PJ_SUCCESS :
							 PJ_ETOOMANY;
}

static pj_status_t add_attribute(
	pj_pool_t *pool, pjmedia_sdp_media *media,
	const struct sippymodem_sdp_audio_description *description,
	enum sippymodem_sdp_audio_attribute kind)
{
	char value[SIPPYMODEM_SDP_PJSIP_ATTRIBUTE_VALUE_MAX];
	enum sippymodem_sdp_result result;
	pjmedia_sdp_attr *attribute;
	pj_str_t text;

	result = sippymodem_sdp_audio_format_attribute(
		description, kind, value, sizeof(value));
	if (result == SIPPYMODEM_SDP_NOT_PRESENT)
		return PJ_SUCCESS;
	if (result != SIPPYMODEM_SDP_OK)
		return PJ_EINVAL;
	text = pj_str(value);
	attribute = pjmedia_sdp_attr_create(
		pool, sippymodem_sdp_audio_attribute_name(kind), &text);
	if (attribute == NULL)
		return PJ_ENOMEM;
	return pjmedia_sdp_media_add_attr(media, attribute);
}

static pj_status_t set_connection(pj_pool_t *pool,
				  pjmedia_sdp_conn **connection,
				  const char *public_address)
{
	pjmedia_sdp_conn *updated;

	if (public_address == NULL || public_address[0] == '\0')
		return PJ_SUCCESS;
	updated = *connection;
	if (updated == NULL) {
		updated = pj_pool_zalloc(pool, sizeof(*updated));
		if (updated == NULL)
			return PJ_ENOMEM;
		*connection = updated;
	}
	updated->net_type = pj_str((char *)"IN");
	updated->addr_type = pj_str(
		strchr(public_address, ':') != NULL ? (char *)"IP6" :
							(char *)"IP4");
	pj_strdup2(pool, &updated->addr, public_address);
	return updated->addr.ptr != NULL ? PJ_SUCCESS : PJ_ENOMEM;
}

static pjmedia_sdp_media *select_audio(pjmedia_sdp_session *session)
{
	for (unsigned int index = 0; index < session->media_count; ++index) {
		pjmedia_sdp_media *media = session->media[index];

		if (media != NULL && media->desc.port != 0 &&
		    pj_text_equals(&media->desc.media, "audio"))
			return media;
	}
	return NULL;
}

static pj_status_t retain_one_audio(pjmedia_sdp_session *session,
				    pj_pool_t *pool,
				    pjmedia_sdp_media *selected,
				    bool is_answer)
{
	if (!is_answer) {
		session->media[0] = selected;
		session->media_count = 1;
		return PJ_SUCCESS;
	}
	for (unsigned int index = 0; index < session->media_count; ++index) {
		pjmedia_sdp_media *media = session->media[index];
		pj_status_t status;

		if (media == NULL || media == selected)
			continue;
		status = pjmedia_sdp_media_deactivate(pool, media);
		if (status != PJ_SUCCESS)
			return status;
	}
	return PJ_SUCCESS;
}

static pj_status_t apply_audio_description(
	pjmedia_sdp_media *media, pj_pool_t *pool,
	const struct sippymodem_sdp_audio_description *description,
	const char *public_address)
{
	char payload[4];
	int payload_size;
	pj_status_t status;

	payload_size = snprintf(payload, sizeof(payload), "%u",
				description->payload_type);
	if (payload_size <= 0 || (size_t)payload_size >= sizeof(payload))
		return PJ_EINVAL;
	media->desc.port = description->rtp_port;
	media->desc.fmt_count = 1;
	pj_strdup2(pool, &media->desc.fmt[0], payload);
	if (media->desc.fmt[0].ptr == NULL)
		return PJ_ENOMEM;
	status = set_connection(pool, &media->conn, public_address);
	if (status != PJ_SUCCESS)
		return status;
	remove_media_owned_attributes(media);
	for (enum sippymodem_sdp_audio_attribute kind =
		     SIPPYMODEM_SDP_AUDIO_RTPMAP;
	     kind < SIPPYMODEM_SDP_AUDIO_ATTRIBUTE_COUNT; ++kind) {
		status = add_attribute(pool, media, description, kind);
		if (status != PJ_SUCCESS)
			return status;
	}
	return PJ_SUCCESS;
}

pj_status_t sippymodem_sdp_pjsip_apply(
	pjmedia_sdp_session *local_sdp, pj_pool_t *pool, bool is_answer,
	const struct sippymodem_sdp_audio_description *description,
	const char *public_address)
{
	pjmedia_sdp_media *audio;
	pj_status_t status;

	if (local_sdp == NULL || pool == NULL || description == NULL ||
	    local_sdp->media_count > PJMEDIA_MAX_SDP_MEDIA ||
	    sippymodem_sdp_audio_encoding_name(description) == NULL)
		return PJ_EINVAL;
	audio = select_audio(local_sdp);
	if (audio == NULL)
		return PJMEDIA_SDPNEG_ENOMEDIA;
	status = preflight_attributes(audio, description);
	if (status != PJ_SUCCESS)
		return status;
	status = retain_one_audio(local_sdp, pool, audio, is_answer);
	if (status != PJ_SUCCESS)
		return status;
	status = set_connection(pool, &local_sdp->conn, public_address);
	if (status != PJ_SUCCESS)
		return status;
	return apply_audio_description(audio, pool, description, public_address);
}
