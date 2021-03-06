#!/usr/bin/env python3

from converter_mock import *
from test_lib import *


class TestOK(TestInstance):
    def run(self):
        class MockConverterOK(MockConverter):
            actions = [RecvSyn(), SendSynAck(payload=Convert().build()), RecvHTTPGet(
            ), SendHTTPResp("HELLO, WORLD!"), SendPkt(flags='RA')]
        MockConverterOK()

    def validate(self):
        self.assert_result("HELLO, WORLD!")


TestOK()
