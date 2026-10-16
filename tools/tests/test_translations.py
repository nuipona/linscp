"""Catalog compiler regressions; no gettext or installed locales required."""
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from compile_translations import (generate, ROOT, OUTPUT, load_catalogs,
                                  gui_page, GUI_PAGE, GUI_DIRECTORY,
                                  check_gui_coverage, check_cli_coverage)


class TranslationTests(unittest.TestCase):
    def test_checked_in_table_is_current(self):
        self.assertEqual(generate(ROOT / 'source/linux/locales'), OUTPUT.read_text(encoding='utf-8'))

    def test_gui_is_current_and_all_source_messages_have_catalogs(self):
        catalogs = load_catalogs(GUI_DIRECTORY)
        page = GUI_PAGE.read_text(encoding='utf-8')
        self.assertEqual(page, gui_page(catalogs, page))
        check_gui_coverage(catalogs, page)
        check_cli_coverage(load_catalogs(ROOT / 'source/linux/locales'))
        with self.assertRaises(ValueError):
            check_gui_coverage(catalogs, page.replace('Site Manager', 'Uncatalogued UI label'))
        with self.assertRaises(ValueError):
            check_gui_coverage(catalogs, page.replace('tn(', 'tn("new singular", "new plural", 2); tn(', 1))

    def test_inline_catalog_cannot_end_the_script(self):
        catalogs = {'en': {'attack': '</script><img src=x onerror=alert(1)>&\u2028'}}
        page = gui_page(catalogs, GUI_PAGE.read_text(encoding='utf-8'))
        data = page.split('const GUI_CATALOGS = ', 1)[1].split(';\n', 1)[0]
        self.assertNotIn('<', data)
        self.assertNotIn('&', data)
        self.assertEqual(json.loads(data), catalogs)

    def test_invalid_catalogs_are_rejected(self):
        invalid = [
            '{"Hello {name}": "Hola"}',
            '{"Hello {name}": "Hola {name} {extra}"}',
            '{"Hello {name}": "Hola {name"}',
            '{"Hello {name}": ""}',
            '{"Hello {name}": 3}',
            '{"Hello {name}": "a", "Hello {name}": "b"}',
            '{"Different": "Diferente"}',
        ]
        with tempfile.TemporaryDirectory() as tmp:
            directory = pathlib.Path(tmp)
            (directory / 'en.json').write_text(json.dumps({'Hello {name}': 'Hello {name}'}))
            for text in invalid:
                with self.subTest(text=text):
                    (directory / 'es.json').write_text(text)
                    with self.assertRaises(ValueError):
                        generate(directory)

    def test_cpp_strings_are_escaped(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = pathlib.Path(tmp)
            text = 'Quote " and newline\n and backslash \\'
            (directory / 'en.json').write_text(json.dumps({text: text}))
            self.assertIn(json.dumps(text), generate(directory))


if __name__ == '__main__':
    unittest.main()
