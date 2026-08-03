// Unit test for GenICam -> flat attribute-tree normalization (no camera).
#include "../src/attribute_tree.hpp"

#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (cond) {                                                       \
            std::printf("PASS: %s\n", #cond);                             \
        } else {                                                          \
            std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

int main() {
    // Already-flat mock-shaped document is a pass-through.
    const std::string flat_in =
        "{\"camera\":\"AinstecMock\",\"attributes\":["
        "{\"key\":\"Exposure\",\"type\":\"int\",\"min\":1,\"max\":100,\"step\":1,\"default\":10}"
        "]}";
    CHECK(acam::flatten_attribute_tree_json(flat_in) == flat_in);

    // Minimal vendor GenICam tree with Category / Integer / Boolean / Enumeration / Float.
    const char* genicam = R"JSON({
      "Description": "3D Camera Attribute Tree.",
      "Feature": [
        {
          "ScanSettings": {
            "Type": "Category",
            "Feature": [
              {
                "TriggerMode": {
                  "AccessMode": "RW",
                  "Type": "Enumeration",
                  "DefaultValue": 0,
                  "EnumEntry": [
                    {"SoftTrigger": {"Value": 0}},
                    {"GPIOTrigger": {"Value": 1}},
                    {"AsyncCalcTrigger": {"Value": 4}}
                  ],
                  "Value": 0
                }
              },
              {
                "Camera": {
                  "Type": "Category",
                  "Feature": [
                    {
                      "Exposure": {
                        "AccessMode": "RW",
                        "Type": "Integer",
                        "Min": 1,
                        "Max": 100,
                        "DefaultValue": 15,
                        "Value": 10
                      }
                    },
                    {
                      "Gain": {
                        "AccessMode": "RW",
                        "Type": "Integer",
                        "Min": 0,
                        "Max": 15,
                        "DefaultValue": 3,
                        "Value": 1
                      }
                    }
                  ]
                }
              }
            ]
          }
        },
        {
          "AlgorithmSettings": {
            "Type": "Category",
            "Feature": [
              {
                "FillHole": {
                  "AccessMode": "RW",
                  "Type": "Boolean",
                  "DefaultValue": false,
                  "Value": true
                }
              },
              {
                "XRangeMin": {
                  "AccessMode": "RW",
                  "Type": "Float",
                  "Min": -10000.0,
                  "Max": 10000.0,
                  "DefaultValue": -10000.0,
                  "Value": -10000.0
                }
              }
            ]
          }
        }
      ],
      "Type": "Category",
      "Version": "1.0"
    })JSON";

    const std::string out = acam::flatten_attribute_tree_json(genicam);
    CHECK(out.find("\"attributes\"") != std::string::npos);
    CHECK(out.find("\"key\":\"Exposure\"") != std::string::npos);
    CHECK(out.find("\"key\":\"Gain\"") != std::string::npos);
    CHECK(out.find("\"key\":\"FillHole\"") != std::string::npos);
    CHECK(out.find("\"key\":\"TriggerMode\"") != std::string::npos);
    CHECK(out.find("\"key\":\"XRangeMin\"") != std::string::npos);
    CHECK(out.find("\"type\":\"bool\"") != std::string::npos);
    CHECK(out.find("\"type\":\"float\"") != std::string::npos);
    // Enumeration -> int with bounds from EnumEntry values (0..4)
    CHECK(out.find("\"key\":\"TriggerMode\",\"type\":\"int\"") != std::string::npos);
    CHECK(out.find("\"min\":0") != std::string::npos);
    CHECK(out.find("\"max\":4") != std::string::npos);
    // Integer gets default step 1 when Step/Inc absent
    CHECK(out.find("\"key\":\"Exposure\",\"type\":\"int\",\"min\":1,\"max\":100,\"step\":1") != std::string::npos);
    // No leftover GenICam scaffolding
    CHECK(out.find("\"Feature\"") == std::string::npos);
    CHECK(out.find("ScanSettings") == std::string::npos);

    // Garbage input is returned unchanged.
    const std::string junk = "not-json";
    CHECK(acam::flatten_attribute_tree_json(junk) == junk);

    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
