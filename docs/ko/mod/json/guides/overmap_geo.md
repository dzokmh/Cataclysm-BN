# 현실 지구 오버맵 패킷

`overmap_geo` 정의는 절차적 노이즈 파이프라인을 실제 세계의 한 지역으로 대체합니다:
실제 지표 피복, 실제 강, 그리고 실제 OSM 도로가 오버맵에 그려지고, 도시는 실제
좌표에 배치됩니다. 이 데이터는 JSON 정의 옆의 하나의 바이너리 사이드카 파일
(`.cmap`)에 담겨 있습니다.

현실 지구 생성은 `GEOGRAPHY_MODE` 월드 옵션 뒤에서 게이트됩니다: 이 옵션이
`real_earth`로 설정된 경우에만 패킷이 사용됩니다. 기본값인 `procedural` 모드는
기존 노이즈 생성기를 그대로 유지합니다.

지역을 만드는 가장 쉬운 방법은 `tools/geo`의 변환 스크립트입니다
([변환기](#변환기) 참조). 이 스크립트는 바운딩 박스에 대해 ESA WorldCover 지표
피복과 OpenStreetMap의 강, 도로, 도시를 다운로드합니다.

## JSON 정의

| 식별자 | 설명 |
| --- | --- |
| id | (_필수_) 패킷의 고유 ID. |
| file | (_필수_) 이 JSON 파일을 기준으로 한 `.cmap` 바이너리 사이드카의 경로. |
| zones | (_임의_) 구역 클래스 인덱스별 생성 규칙 (`"0"`..`"255"`, 십진수 키). |
| river | (_임의_) 패킷의 강 폴리라인을 그리는 방법. 생략하면 "강 없음"을 의미합니다. |
| cities | (_임의_) 실제 세계의 도시 배치. 좌표는 패킷 앵커를 기준으로 하며, 강 정점과 같은 좌표 공간을 사용합니다. |

```json
[
  {
    "type": "overmap_geo",
    "id": "geo_new_england",
    "file": "geo_new_england.cmap",
    "zones": {
      "1": { "paint": "forest" },
      "2": { "paint": "lake_surface" },
      "3": { "paint": "forest_water" }
    },
    "river": { "paint": "river_center", "width": 3 },
    "cities": [
      { "name": "Boston", "x": 5964, "y": 2497, "size": 16, "finale": true }
    ]
  }
]
```

### zones

각 멤버는 바이너리 마스크의 구역 클래스 인덱스입니다. 현재 한 멤버가 인식됩니다:

| 식별자 | 설명 |
| --- | --- |
| paint | (_임의_) 해당 구역이 덮는 모든 셀에 칠해지는 기본 오버맵 지형. 생략하면 해당 구역은 기본 지형을 그대로 두고, 기존 절차적 패스가 세부 텍스처를 계속 제공합니다. |

규칙이 없는 구역 클래스 역시 칠해지지 않은 채 남겨집니다.

### river

| 식별자 | 설명 |
| --- | --- |
| paint | (_필수_) 강 폴리라인을 따라 칠해지는 오버맵 지형. |
| width | (_임의_) OMT 셀 단위의 기본 강 폭. 기본값은 1, 최소값은 1입니다. 자체 폭을 가지지 않는 강에만 사용되며, 변환기는 OSM `width` 태그에서 강별 폭을 수집해 바이너리 패킷에 저장합니다. |

### cities

도시 씨드의 배열입니다:

| 식별자 | 설명 |
| --- | --- |
| name | (_임의_) 도시 레이블로 표시되는 실제 지명. |
| x, y | (_필수_) 도시 중심, 패킷 앵커(마스크의 북서쪽 모서리) 기준 OMT 셀 단위. |
| size | (_임의_) `CITY_SIZE` 굴림과 마찬가지로 OMT 셀 단위의 바닐라 도시 반경, 1..16. 기본값은 1. |
| finale | (_임의_) 여기서 특별한 파이널 배치(vault 등)를 시도할지 여부. 기본값은 false. |

도시는 실제 좌표에 기존 절차적 도시 머신러니로 배치됩니다. 패킷이 도시 가이던스를
제공하는 동안에는 바닐라 `place_isolated_cities` 패스가 건너뛰어 절차적 도시가
실제 지리 위에 겹쳐 나타나지 않습니다.

## 바이너리 패킷 포맷 (`.cmap`)

사이드카는 `src/overmap_geo.cpp`의 `overmap_geo::parse_binary`가 파싱하는
리틀 엔디안 스트림입니다. 현재 버전은 3이며, 이전 버전은 로드가 거부되므로 해당
지역은 변환기로 다시 생성해야 합니다. 변환기는 또한 긴 소스 폴리라인을
Douglas-Peucker(OMT 0.5, 즉 12 m 허용 오차)로 얇게 단순화하고 정점 델타를
zigzag varint로 저장하므로, 뉴잉글랜드 규모 리전도 패킷이 약 1 MB 안에 들어갑니다.

1. `u32` 매직 `"BNCG"`
2. `u16` 포맷 버전 (3)
3. `i32` 앵커 X, 앵커 Y — 첫 번째(북서쪽) 마스크 셀의 절대 OMT 좌표; 파일의 모든
   정점 좌표는 이 앵커를 기준으로 한 델타로 주어집니다
4. `u16` 셀 스팬 — 하나의 마스크 매크로 셀 변이 덮는 OMT 셀 수
   (변환기는 8을 사용합니다, 즉 8×8 OMT = 각 24 m = 매크로 셀당 192 m)
5. `u32` 마스크 셀 X, Y — 매크로 셀 단위의 마스크 크기
6. `u16` 구역 개수 — 구역 클래스 테이블의 크기
7. `u16` 강 개수
8. 런 렝스 인코딩된 마스크, 북서쪽에서 행 방향: `[u8 실행 길이, u8 구역 인덱스]`
   바이트 쌍의 반복으로 `cells_x * cells_y`개 셀이 채워질 때까지 진행;
   길이 0 런과 정의되지 않은 구역 인덱스는 거부됩니다
9. 강 폴리라인이 차례로 이어집니다: `u16` 정점 개수 (2 이상), `u8` 강 폭
   (OMT 셀 단위, 0 = JSON `river` 룰 폭을 폴백으로 사용), 그리고 정점마다
   zigzag-varint X 델타와 Y 델타. 첫 번째 델타는 앵커 기준, 이후는
   이전 정점 기준입니다
10. 도로 폴리라인: `u32` 도로 개수, 그리고 각 도로마다 `u8` 도로 티어
    (0..2, 2보다 큰 값은 거부됨), zigzag-varint 정점 개수 (2 이상), 그리고
    정점마다 zigzag-varint X 델타와 Y 델타 (첫 번째는 앵커 기준, 이후는
    이전 정점 기준)

마지막 섹션 뒤의 잉여 바이트는 거부됩니다. zigzag varint는 부호 있는 델타를
음수가 아닌 LEB128 base-128 수 (0, -1, 1, -2, 2 ... 는 0, 1, 2, 3, 4 ... 로)
로 저장하므로, 부드러운 폴리라인은 좌표당 1-2바이트면 충분합니다.

도로 티어는 해당 way의 OSM `highway` 클래스를 인코딩합니다:

| 티어 | 값 | OSM 클래스 |
| --- | --- | --- |
| highway | 0 | `motorway`, `trunk`, `_link` 포함 |
| main | 1 | `primary`, `secondary`, `_link` 포함 |
| local | 2 | `tertiary`, `_link` 포함 |

## 생성 동작

패킷은 해당 오버맵과 교차할 때만, 그리고 `GEOGRAPHY_MODE` 월드 옵션이
`real_earth`로 설정된 경우에만 사용됩니다. 커버된 오버맵 내부에서:

* 구역은 규칙이 요청하는 대로 칠해집니다; 칠해지지 않은 클래스는 기본 지형과
  절차적 세부 텍스처를 유지합니다.
* 커버된 오버맵에서는 바닐라 강 및 호수 노이즈 패스가 건너뛰어지며, 대신 패킷의
  강이 사용됩니다. 폴리라인은 강별 폭으로 칠해지고 (폭이 없는 강은 `river` 룰
  폭으로 폴백) 바닐라 강처럼 다듬어집니다.
* 도로 폴리라인은 셀 단위로 래스터화됩니다. 물 위의 도로는 다리가 됩니다: 다리
  구간은 높은 다리 지형으로 올라가 양쪽 끝에 램프가 놓이고, 상판 아래의 물은
  under-bridge 지형이 됩니다. 어떤 다리 구간이 물을 사선으로 건넌다면, 바닐라의
  bridgehead와 램프 로직이 깔끔하게 적용되도록 축 방향(맨해튼) 경로로 재구성됩니다.
* 커버된 오버맵에서는 바닐라 `place_roads`가 건너뛰어지므로 절차적 도로가 실제
  도로망을 가로지르지 않습니다.
* 커버된 오버맵 가장자리의 도로 끝은 `local_road`의 `connections_out` 항목으로
  노출됩니다. 따라서 인접한 절차적 오버맵이 자기 경계에서 실제 도로를 이어받아
  바깥으로 확장합니다.
* 큰 절차적 도시 반경(도시 씨드와 반경 8 이상의 이웃) 내부에서는 `local` 티어의
  도로가 제거되어 절차적 거리 격자가 그 지역을 담당합니다. 더 높은 티어는
  통과합니다.
* 패킷의 도시는 바닐라 도시 머신러니로 배치되며, 패킷이 도시 가이던스를 제공하는
  동안 바닐라 `place_isolated_cities` 패스는 비활성화됩니다.

## 변환기

변환기는 `tools/geo/make_geo_packet.py`에 있으며 `rasterio`가 있는 Python
환경이 필요합니다:

```sh
python3 -m venv .venv && .venv/bin/pip install rasterio
.venv/bin/python tools/geo/make_geo_packet.py \
  --bbox -72.80 42.03 -70.95 42.90 \
  --out-dir ~/.local/share/cataclysm-bn/mods/geo_new_england \
  --mod-id geo_new_england
```

| 인수 | 설명 |
| --- | --- |
| --bbox | (_필수_) 지역 바운딩 박스, 도 단위의 `서쪽 남쪽 동쪽 북쪽`. 박스의 중심은 절대 OMT (0, 0), 즉 월드 스폰 지점에 앵커링됩니다. |
| --out-dir | (_필수_) 출력 모드 디렉터리; `.cmap` 사이드카, 모드 JSON, `.ppm` 프리뷰를 받습니다. |
| --mod-id | 모드 식별자, 생성된 JSON에 사용됩니다. 기본값은 `geo_new_england`입니다. |
| --cache-dir | 다운로드된 타일과 Overpass 응답의 캐시. 기본값은 `~/.cache/cbn-geo`입니다. |
| --no-rivers | Overpass 강/도시 쿼리를 건너뛰고 `river`/`cities` 멤버를 쓰지 않습니다. |

`.ppm` 프리뷰(P6, 북쪽 위)는 사람이 빠르게 확인하기 위한 것입니다:
`ffmpeg -i x_preview.ppm x.png`로 변환하세요.

도로와 강 폴리라인은 모두 인코딩 전에 Douglas-Peucker(OMT 0.5, 12 m 허용
오차)로 얇게 단순화되고, 강 폭은 태그가 있으면 OSM `width` 태그에서 수집됩니다.

데이터 소스와 라이선스:

* 지표 피복 마스크: [ESA WorldCover 10 m 2021 v200](https://esa-worldcover.org)
  3°x3° COG 타일 (공개 S3 버킷, 필요 시 다운로드), 8×8 OMT (192 m) 구역 셀로
  다수결 샘플링. © ESA WorldCover project 2021 / Contains modified Copernicus
  Sentinel data (2021) processed by ESA WorldCover consortium —
  [CC-BY 4.0](https://creativecommons.org/licenses/by/4.0/).
* 강(`waterway=river|canal` 폴리라인)과 도시/마을 노드: Overpass API를 통해
  OpenStreetMap에서 가져옴. © OpenStreetMap contributors,
  [ODbL](https://opendatacommons.org/licenses/odbl/).
* 도로(`highway`가 `motorway|trunk|primary|secondary|tertiary`, `_link` 포함):
  Overpass API를 통해 OpenStreetMap에서 가져옴, 같은 ODbL 조건이 적용됩니다.
