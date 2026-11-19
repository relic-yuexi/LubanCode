"""Pure request-contract checks; no local HTTP or native process is started."""
import json
import unittest

from scripts.ci.run_web_search_consumer import validate_wire


class WireContractTests(unittest.TestCase):
    def test_brave_encoding_and_auth_are_both_required(self):
        target = "/provider/1?q=a%26b%20%E4%B8%AD%E6%96%87&count=2"
        headers = {"X-Subscription-Token": "fixture-search-key"}
        self.assertTrue(validate_wire("GET", target, headers, b"")[1])
        self.assertFalse(validate_wire("GET", target.replace("%26", "&"), headers, b"")[1])
        self.assertFalse(validate_wire("GET", target, {}, b"")[1])
        self.assertFalse(validate_wire("POST", target, headers, b"")[1])

    def test_resumed_credential_is_fresh(self):
        body = json.dumps({"query": "a&b 中文", "max_results": 2}).encode()
        self.assertEqual(validate_wire("POST", "/resume", {"Authorization": "Bearer resumed-search-key"}, body),
                         ("/resume", True, True))
        self.assertEqual(validate_wire("POST", "/resume", {"Authorization": "Bearer fixture-search-key"}, body),
                         ("/resume", True, False))

    def test_live_session_credential_and_budget_must_match(self):
        for key, count in (("fixture-search-key", 1), ("isolated-search-key", 2)):
            header = {"Authorization": "Bearer " + key}
            body = json.dumps({"query": "a&b 中文", "max_results": count}).encode()
            self.assertTrue(validate_wire("POST", "/isolation", header, body)[1])
            wrong = json.dumps({"query": "a&b 中文", "max_results": 3 - count}).encode()
            self.assertFalse(validate_wire("POST", "/isolation", header, wrong)[1])

    def test_serper_cannot_use_tavily_shape(self):
        header = {"X-API-KEY": "fixture-search-key"}
        good = json.dumps({"q": "a&b 中文", "num": 2}).encode()
        self.assertTrue(validate_wire("POST", "/provider/2", header, good)[1])
        bad = json.dumps({"query": "a&b 中文", "max_results": 2}).encode()
        self.assertFalse(validate_wire("POST", "/provider/2", header, bad)[1])
        self.assertFalse(validate_wire("POST", "/provider/2", header, b"[]")[1])


if __name__ == "__main__":
    unittest.main()
