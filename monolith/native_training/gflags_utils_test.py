# Copyright 2022 ByteDance and/or its affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from absl import flags
from absl.testing import absltest
from typing import get_type_hints
from enum import Enum
import dataclasses

from monolith.native_training import gflags_utils as utils

FLAGS = flags.FLAGS


@dataclasses.dataclass
class TestConfig:
  """
    :param test_int1: integer 1 for test
    :param test_int2: integer 2 for test
    :param test_str: string for test
                        and test another line
  """
  test_int1: int = 0
  test_int2: int = 0
  test_str: str = None


@utils.extract_flags_decorator()
@dataclasses.dataclass
class TestConfig2:
  """
    :param testconfig2_int1: integer 1 for TestConfig2
    :param testconfig2_str1: str 1 for TestConfig2
  """
  testconfig2_int1: int = 1
  testconfig2_str1: str = "str1"


@utils.extract_flags_decorator({"testconfig3_int1"})
@dataclasses.dataclass
class TestConfig3:
  """
    :param testconfig3_int1: testconfig3_int1
    :param testconfig3_int2: testconfig3_int2
    :param testconfig3_str1: testconfig3_str1
  """
  testconfig3_int1: int = 1
  testconfig3_int2: int = 2
  testconfig3_str1: str = "str1"


@dataclasses.dataclass
class TestConfig4(TestConfig):
  """
    :param testconfig4_int1: testconfig4_int1
    :param testconfig4_str1: testconfig4_str1
  """
  testconfig4_int1: int = 4
  testconfig4_str1: str = "testconfig4_str1"


@dataclasses.dataclass
class TestConfig5Base:
  """
    :param testconfig5base_int1: testconfig5base_int1
    :param testconfig5base_int2: testconfig5base_int2
    :param testconfig5base_str: testconfig5base_str
  """
  testconfig5base_int1: int = 0
  testconfig5base_int2: int = 0
  testconfig5base_str: str = None


@utils.extract_flags_decorator(is_nested=False)
@dataclasses.dataclass
class TestConfig5(TestConfig5Base):
  """
    :param testconfig5_int1: testconfig5_int1
    :param testconfig5_str1: testconfig5_str1
  """
  testconfig5_int1: int = 5
  testconfig5_str1: str = "testconfig5_str1"


class GflagUtilsTest(absltest.TestCase):

  def _check_help_info(self, cls, skip_flags=set()):
    help_info = utils.extract_help_info(cls)
    for key, _ in get_type_hints(cls).items():
      if key in skip_flags:
        continue
      self.assertIn(key, help_info,
                    '{} is not in {}, please add a help info'.format(key, cls))

  def test_extract_help_info(self):

    res = utils.extract_help_info(TestConfig)
    self.assertIn("test_int1", res)
    self.assertIn("test_int2", res)
    self.assertIn("test_str", res)
    self.assertEqual("integer 1 for test", res["test_int1"])
    self.assertEqual("integer 2 for test", res["test_int2"])
    self.assertEqual("string for test and test another line", res["test_str"])

    res2 = utils.extract_help_info(TestConfig4, is_nested=False)
    self.assertIn("testconfig4_int1", res2)
    self.assertIn("testconfig4_str1", res2)
    self.assertNotIn("test_int1", res2)
    self.assertNotIn("test_int2", res2)
    self.assertNotIn("test_str", res2)

  def test_update(self):
    FLAGS = flags.FLAGS
    flags.DEFINE_integer("test_int1", 2, "test int 1")
    flags.DEFINE_integer("test_int2", 3, "test int 2")

    config = TestConfig(
        test_int1=1,
        test_int2=0,
    )
    utils.update(config)
    # will not update test_int1 because test_int1 in config is not default value.
    # will update test_int2 because test_int2 in config is default value
    #      and FLAGS.test_int2 is not default value.
    self.assertEqual(config.test_int1, 1)  #not updated
    self.assertEqual(config.test_int2, 3)  #updated
    self.assertEqual(config.test_str, None)
    # for test_str attr, no FLAGS is define, so nothing will happend

  def test_extract_gflags_decorator(self):
    FLAGS = flags.FLAGS
    conf = TestConfig2(testconfig2_int1=2, testconfig2_str1="newstr1")
    self.assertEqual(FLAGS.testconfig2_int1, 1)
    self.assertEqual(FLAGS.testconfig2_str1, "str1")
    self.assertEqual(conf.testconfig2_int1, 2)
    self.assertEqual(conf.testconfig2_str1, "newstr1")

    conf3 = TestConfig3()
    self.assertEqual(hasattr(FLAGS, "testconfig3_int1"), False)
    self.assertEqual(hasattr(FLAGS, "testconfig3_int2"), True)
    self.assertEqual(hasattr(FLAGS, "testconfig3_str1"), True)

    self.assertEqual(hasattr(conf3, "testconfig3_int1"), True)
    self.assertEqual(hasattr(conf3, "testconfig3_int2"), True)
    self.assertEqual(hasattr(conf3, "testconfig3_str1"), True)

    conf5 = TestConfig5()
    self.assertEqual(hasattr(FLAGS, "testconfig5_int1"), True)
    self.assertEqual(hasattr(FLAGS, "testconfig5_str1"), True)
    self.assertEqual(hasattr(FLAGS, "testconfig5base_int1"), False)
    self.assertEqual(hasattr(FLAGS, "testconfig5base_int2"), False)
    self.assertEqual(hasattr(FLAGS, "testconfig5base_str"), False)

    self.assertEqual(hasattr(conf5, "testconfig5_int1"), True)
    self.assertEqual(hasattr(conf5, "testconfig5_str1"), True)
    self.assertEqual(hasattr(conf5, "testconfig5base_int1"), True)
    self.assertEqual(hasattr(conf5, "testconfig5base_int2"), True)
    self.assertEqual(hasattr(conf5, "testconfig5base_str"), True)

  flags.DEFINE_string("testflag6", "", "")

  @utils.update_by_flags
  @utils.LinkDataclassToFlags(linked_list=["testflag6"],
                              linked_map={"v": "testflag6"})
  @dataclasses.dataclass
  class TestClass6:
    v: str = None
    testflag6: str = None

  def test_link_flag(self):
    FLAGS.testflag6 = ""
    c = self.TestClass6()
    self.assertEqual(c.v, None)
    self.assertEqual(c.testflag6, None)

    FLAGS.testflag6 = "a"
    c = self.TestClass6()
    self.assertEqual(c.v, "a")
    self.assertEqual(c.testflag6, "a")

    FLAGS.testflag6 = "b"
    c = self.TestClass6(v="v")
    self.assertEqual(c.v, "v")
    self.assertEqual(c.testflag6, "b")

  flags.DEFINE_string("testflag7", "", "")

  @utils.LinkDataclassToFlags(linked_map={"v": "testflag7"})
  @dataclasses.dataclass
  class TestClass7Base:
    v: str = None

  @utils.update_by_flags
  @dataclasses.dataclass
  class TestClass7Inherit(TestClass7Base):
    v2: str = "v2"

  def test_link_flag_inheritance(self):
    FLAGS.testflag7 = "a"
    c = self.TestClass7Inherit()
    self.assertEqual(c.v, "a")
    self.assertEqual(c.v2, "v2")


if __name__ == "__main__":
  absltest.main()
