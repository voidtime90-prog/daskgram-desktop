/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lang/translate_provider.h"

#include "base/options.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_msg_id.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "lang/translate_mtproto_provider.h"
#include "lang/translate_url_provider.h"
#include "spellcheck/spellcheck_types.h"
#include "platform/platform_translate_provider.h"

#include <memory>

namespace {

// DaskGram: Google Translate by default (free public endpoint, no API key).
//
// client=dict-chrome-ex matters: the classic client=gtx is answered with
// HTTP 429 ("Sorry...") from ordinary IPs, while dict-chrome-ex returns the
// segmented array ParseSegmentedArrayResponse expects:
//   [[["text","source",null,null,3]],null,"en"]
const auto kDaskGramTranslateUrl = QString::fromLatin1(
	"https://translate.googleapis.com/translate_a/single"
	"?client=dict-chrome-ex&dt=t&sl=%f&tl=%t&q=%q");

base::options::option<QString> OptionTranslateUrlTemplate({
	.id = "translate-url-template",
	.name = "Translate URL template",
	.description = "Template URL for custom translation provider."
		" Supports %q text, %f source language and %t target language.",
});

} // namespace

namespace Ui {

namespace {

// DaskGram: try the direct Google Translate request first; if it yields no
// text (the free endpoint is rate-limited from some networks, or the request
// cannot reach Google at all), retry through our own server, which translates
// with the same backend. The base requestBatch() routes every item through
// request(), so the batched path keeps this behaviour as well.
class GoogleThenServerProvider final : public TranslateProvider {
public:
	GoogleThenServerProvider(
		std::unique_ptr<TranslateProvider> direct,
		std::unique_ptr<TranslateProvider> server)
	: _direct(std::move(direct))
	, _server(std::move(server)) {
	}

	[[nodiscard]] bool supportsMessageId() const override {
		return false;
	}

	void request(
			TranslateProviderRequest request,
			LanguageId to,
			Fn<void(TranslateProviderResult)> done) override {
		const auto server = _server.get();
		_direct->request(request, to, [=](
				TranslateProviderResult result) mutable {
			if (result.error == TranslateProviderError::None
				&& result.text.has_value()
				&& !result.text->text.isEmpty()) {
				done(std::move(result));
			} else {
				server->request(request, to, done);
			}
		});
	}

private:
	const std::unique_ptr<TranslateProvider> _direct;
	const std::unique_ptr<TranslateProvider> _server;

};

} // namespace

std::unique_ptr<TranslateProvider> CreateTranslateProvider(
		not_null<Main::Session*> session) {
	// DaskGram: fall back to Google Translate when no custom template is set.
	// The direct Google request is tried first, with our own server as a
	// backstop for the networks where the free endpoint is not usable.
	const auto custom = OptionTranslateUrlTemplate.value();
	const auto urlTemplate = custom.isEmpty()
		? kDaskGramTranslateUrl
		: custom;
	if (urlTemplate.contains(u"%q"_q)) {
		return std::make_unique<GoogleThenServerProvider>(
			CreateUrlTranslateProvider(urlTemplate),
			CreateMTProtoTranslateProvider(session));
	}
	if (Core::App().settings().usePlatformTranslation()
		&& Platform::IsTranslateProviderAvailable()) {
		return Platform::CreateTranslateProvider();
	}
	return CreateMTProtoTranslateProvider(session);
}

TranslateProviderRequest PrepareTranslateProviderRequest(
		not_null<TranslateProvider*> provider,
		not_null<PeerData*> peer,
		MsgId msgId,
		TextWithEntities text) {
	auto result = TranslateProviderRequest{
		.peerId = uint64(peer->id.value),
		.msgId = IsServerMsgId(msgId) ? msgId.bare : 0,
		.text = std::move(text),
	};
	if (provider->supportsMessageId()) {
		return result;
	}
	if (result.msgId) {
		if (const auto i = peer->owner().message(peer, MsgId(result.msgId))) {
			result.text = i->originalText();
		}
		result.msgId = 0;
	}
	return result;
}

} // namespace Ui
