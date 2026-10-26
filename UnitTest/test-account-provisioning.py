"""Native integration fixture provisioning across old and email-policy servers."""
import importlib.util
import json
from pathlib import Path
import unittest
import urllib.error

spec = importlib.util.spec_from_file_location("remote_server_fixture", Path(__file__).with_name("remote_server_integration.py"))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


class Response:
    def __init__(self, value): self.value = value
    def __enter__(self): return self
    def __exit__(self, *_): pass
    def read(self): return json.dumps(self.value).encode()


class Opener:
    def __init__(self, status): self.status, self.requests, self.probes = status, [], 0
    def open(self, request, timeout):
        if request.full_url.endswith("/registration/config"):
            self.probes += 1
            if self.status != 200:
                raise urllib.error.HTTPError(request.full_url, self.status, "fixture", {}, None)
            return Response({"enabled": False, "mail_ready": False})
        self.requests.append(json.loads(request.data))
        return Response({"created": True})


class ProvisioningTests(unittest.TestCase):
    def test_current_requires_explicit_quota_and_exempts_fixture_uploads(self):
        account = fixture.Account("http://fixture.invalid")
        account.http = Opener(200)
        body = {"username": "fixture"}
        account.call("admin/users", "POST", body)
        account.call("admin/users", "POST", {"username": "second", "quota_bytes": 1024, "email_verification_required": True})
        self.assertEqual(account.http.requests, [
            {"username": "fixture", "quota_bytes": 0, "email_verification_required": False},
            {"username": "second", "quota_bytes": 1024, "email_verification_required": True}])
        self.assertEqual(body, {"username": "fixture"})
        self.assertEqual(account.http.probes, 1)

    def test_old_server_keeps_its_strict_creation_contract(self):
        for status in (401, 404):
            account = fixture.Account("http://fixture.invalid")
            account.http = Opener(status)
            account.call("admin/users", "POST", {"username": "fixture"})
            self.assertEqual(account.http.requests, [{"username": "fixture"}])

    def test_unknown_probe_failures_do_not_silently_disable_policy_fields(self):
        account = fixture.Account("http://fixture.invalid")
        account.http = Opener(503)
        with self.assertRaises(urllib.error.HTTPError): account.call("admin/users", "POST", {"username": "fixture"})
        self.assertEqual(account.http.requests, [])


if __name__ == "__main__": unittest.main()
