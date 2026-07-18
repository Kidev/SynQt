# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Resolution of ``build.loading``: the page every visitor sees while the client loads."""

import re
import tempfile
import unittest
from pathlib import Path

import yaml

from synqt import loadingpage


def _config(**loading):
    return {"project": {"name": "app"}, "build": {"loading": dict(loading)}}


class ResolveTest(unittest.TestCase):
    def test_defaults_need_no_config(self):
        self.assertEqual(loadingpage.background({}), loadingpage.DEFAULT_BACKGROUND)
        self.assertEqual(loadingpage.title({}), loadingpage.DEFAULT_TITLE)
        self.assertIsNone(loadingpage.html_override({}, Path(".")))

    def test_default_background_is_the_site_gradient(self):
        self.assertIn("linear-gradient(165deg, #201335 0%", loadingpage.DEFAULT_BACKGROUND)

    def test_keys_override_the_defaults(self):
        config = _config(background="#000", title="Acme")
        self.assertEqual(loadingpage.background(config), "#000")
        self.assertEqual(loadingpage.title(config), "Acme")

    def test_blank_values_fall_back_to_the_defaults(self):
        config = _config(background="   ", title="")
        self.assertEqual(loadingpage.background(config), loadingpage.DEFAULT_BACKGROUND)
        self.assertEqual(loadingpage.title(config), loadingpage.DEFAULT_TITLE)

    def test_default_logo_is_the_packaged_synqt_mark(self):
        markup = loadingpage.logo_svg({}, Path("."))
        self.assertTrue(markup.startswith("<svg"))
        self.assertIn("</svg>", markup)

    def test_default_logo_is_the_variant_that_reads_on_the_dark_default(self):
        # The mark ships in two variants and the naming is a trap. The light-background
        # one paints its wordmark in dark teal #00414a, which all but vanishes on the
        # default gradient; the site hit the same trap and documented it in
        # overrides/partials/header.html. The default background here is dark, so the
        # default logo must be the reversed variant. Checked against paint attributes
        # rather than the raw text, which mentions the colour in a comment.
        markup = loadingpage.logo_svg({}, Path("."))
        paints = [paint.lower()
                  for paint in re.findall(r'(?:fill|stroke)="([^"]+)"', markup)]
        self.assertTrue(paints)
        self.assertNotIn("#00414a", paints)

    def test_logo_key_inlines_the_projects_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "assets").mkdir()
            (root / "assets" / "acme.svg").write_text(
                '<?xml version="1.0"?>\n<svg id="acme"></svg>\n')
            markup = loadingpage.logo_svg(_config(logo="assets/acme.svg"), root)
        # The XML prolog must be stripped: inlining it into HTML is a parse error.
        self.assertTrue(markup.startswith("<svg"))
        self.assertIn('id="acme"', markup)
        self.assertNotIn("<?xml", markup)

    def test_default_favicon_is_a_self_contained_data_uri(self):
        import base64

        uri = loadingpage.favicon_data_uri({}, Path("."))
        self.assertTrue(uri.startswith("data:image/svg+xml;base64,"))
        markup = base64.b64decode(uri.split(",", 1)[1]).decode("utf-8")
        self.assertIn("<svg", markup)
        # Squared and tinted for a dark tab, like the header hero: no invisible teal.
        paints = [paint.lower()
                  for paint in re.findall(r'(?:fill|stroke)="([^"]+)"', markup)]
        self.assertTrue(paints)
        self.assertNotIn("#00414a", paints)

    def test_icon_key_inlines_the_projects_file(self):
        import base64

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "assets").mkdir()
            (root / "assets" / "acme-icon.svg").write_text('<svg id="acme-icon"></svg>\n')
            uri = loadingpage.favicon_data_uri(_config(icon="assets/acme-icon.svg"), root)
        markup = base64.b64decode(uri.split(",", 1)[1]).decode("utf-8")
        self.assertIn('id="acme-icon"', markup)

    def test_html_override_resolves_against_the_project(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = loadingpage.html_override(_config(html="client/loading.html"), root)
        self.assertEqual(path, root / "client" / "loading.html")


class ShellTest(unittest.TestCase):
    def _shell(self, config=None, project_dir=None):
        from synqt import appgen
        return appgen.render_client_shell("client.js", config or {},
                                          project_dir or Path("."))

    def test_default_shell_inlines_the_mark_and_the_gradient(self):
        shell = self._shell()
        self.assertIn("<svg", shell)
        self.assertIn(loadingpage.DEFAULT_BACKGROUND, shell)

    def test_shell_references_no_file_the_bundle_lacks(self):
        # Regression guard. The old shell pointed at qtlogo.svg, which no SynQt build
        # ever places in the bundle, so every client showed a broken image while loading.
        shell = self._shell()
        self.assertNotIn("qtlogo.svg", shell)
        for name in re.findall(r'src="([^"]+)"', shell):
            self.assertIn(name, ("client.js", "qtloader.js", "synqt-boot.js"), name)

    def test_shell_carries_the_synqt_favicon(self):
        # The browser tab must show the SynQt mark, not Qt's default. The icon is inlined as a
        # data: URI (admitted by the default edge img-src 'self' data:), so the tab paints with
        # no extra request.
        shell = self._shell()
        self.assertIn('<link rel="icon" type="image/svg+xml" '
                      'href="data:image/svg+xml;base64,', shell)

    def test_shell_carries_the_boot_hook_ids(self):
        shell = self._shell()
        for hook in ("synqt-loading", "synqt-bar", "synqt-status", "screen"):
            self.assertIn('id="%s"' % hook, shell)

    def test_shell_honors_the_config(self):
        shell = self._shell(_config(background="#000", title="Acme"))
        self.assertIn("#000", shell)
        self.assertIn("<title>Acme</title>", shell)

    def test_title_is_escaped(self):
        shell = self._shell(_config(title='A<script>"&'))
        self.assertNotIn("<script>", shell)
        self.assertIn("&lt;script&gt;", shell)

    def test_html_override_replaces_the_page(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "client").mkdir()
            (root / "client" / "loading.html").write_text("<!doctype html><p>mine</p>")
            shell = self._shell(_config(html="client/loading.html"), root)
        self.assertEqual(shell, "<!doctype html><p>mine</p>")


class BootTest(unittest.TestCase):
    def _boot(self):
        from synqt import appgen
        return appgen.render_boot_js("client", {})

    def test_hands_qt_a_streaming_compiled_module(self):
        boot = self._boot()
        # qt.module is documented qtloader API (Promise<WebAssembly.Module>), which is
        # what buys determinate progress without giving up streaming compilation.
        self.assertIn("module:", boot)
        self.assertIn("WebAssembly.compileStreaming", boot)

    def test_progress_uses_the_manifest_size_not_content_length(self):
        boot = self._boot()
        self.assertIn("wasm_size", boot)
        self.assertNotIn("Content-Length", boot)
        self.assertNotIn("content-length", boot)

    def test_drives_the_shell_hooks_and_entry(self):
        boot = self._boot()
        for hook in ("synqt-loading", "synqt-bar", "synqt-status", "screen"):
            self.assertIn(hook, boot)
        self.assertIn("client_entry", boot)

    def test_is_csp_clean(self):
        boot = self._boot()
        self.assertNotIn("eval(", boot)
        self.assertNotIn("innerHTML", boot)

    def test_carries_the_spdx_header(self):
        self.assertIn("SPDX-License-Identifier: Apache-2.0", self._boot())


class CheckTest(unittest.TestCase):
    def _validate(self, loading):
        from synqt import check
        config = {"project": {"name": "app"},
                  "entities": [{"name": "client", "kind": "client", "targets": ["wasm"]},
                               {"name": "web", "kind": "service", "capability": "web_edge"}],
                  "build": {"loading": loading}}
        return check.validate(config)

    def test_a_well_formed_loading_block_is_accepted(self):
        ok, errors = self._validate({"logo": "assets/acme.svg", "title": "Acme"})
        self.assertTrue(ok, errors)

    def test_unknown_keys_are_rejected(self):
        ok, errors = self._validate({"colour": "#000"})
        self.assertFalse(ok)
        self.assertTrue(any("colour" in error for error in errors), errors)

    def test_non_string_values_are_rejected(self):
        ok, errors = self._validate({"title": 7})
        self.assertFalse(ok)
        self.assertTrue(any("title" in error for error in errors), errors)

    def test_html_override_and_keys_together_are_rejected(self):
        # The override replaces the whole page, so the keys would be silently ignored.
        ok, errors = self._validate({"html": "client/loading.html", "title": "Acme"})
        self.assertFalse(ok)
        self.assertTrue(any("html" in error for error in errors), errors)

    def test_every_message_is_prefixed_so_it_can_fail_the_build(self):
        # validate() derives ok from the "error:" prefix, so an unprefixed message is a
        # check that silently never fails.
        ok, errors = self._validate({"colour": "#000"})
        self.assertFalse(ok)
        for error in errors:
            self.assertTrue(error.startswith(("error:", "warn:", "ok:")), error)


class LintLoadingTest(unittest.TestCase):
    def _lint(self, loading, make=()):
        from synqt import check
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "synqt.yaml").write_text(yaml.safe_dump(
                {"project": {"name": "app"}, "build": {"loading": loading}}))
            for name, body in make:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(body)
            return check.lint_loading(root)

    def test_no_loading_block_is_silent(self):
        self.assertEqual(self._lint({}), [])

    def test_missing_logo_file_is_an_error(self):
        messages = self._lint({"logo": "assets/nope.svg"})
        self.assertTrue(any(m.startswith("error:") and "nope.svg" in m for m in messages),
                        messages)

    def test_present_logo_file_is_accepted(self):
        self.assertEqual(
            self._lint({"logo": "assets/acme.svg"}, [("assets/acme.svg", "<svg/>")]), [])

    def test_missing_html_override_is_an_error(self):
        messages = self._lint({"html": "client/loading.html"})
        self.assertTrue(any(m.startswith("error:") and "loading.html" in m
                            for m in messages), messages)

    def test_html_override_missing_a_boot_hook_is_an_error(self):
        # The override must keep its contract with the boot script or the app never shows.
        page = '<!doctype html><div id="synqt-loading"></div><div id="screen"></div>'
        messages = self._lint({"html": "client/loading.html"},
                              [("client/loading.html", page)])
        self.assertTrue(any(m.startswith("error:") and "synqt-bar" in m for m in messages),
                        messages)

    def test_complete_html_override_is_accepted(self):
        page = ('<!doctype html><div id="synqt-loading">'
                '<div id="synqt-bar"></div><div id="synqt-status"></div></div>'
                '<div id="screen"></div><script src="synqt-boot.js"></script>')
        self.assertEqual(self._lint({"html": "client/loading.html"},
                                    [("client/loading.html", page)]), [])


if __name__ == "__main__":
    unittest.main()
