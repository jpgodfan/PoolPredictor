#include <jni.h>
#include <android/log.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#define LOG_TAG "PoolPredictorNative"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int REFERENCE_WIDTH = 1612;
constexpr int REFERENCE_HEIGHT = 720;
constexpr int REFERENCE_ROI_X = 322;
constexpr int REFERENCE_ROI_Y = 172;
constexpr int REFERENCE_ROI_W = 970;
constexpr int REFERENCE_ROI_H = 485;

constexpr double BASE_LINE_THRESHOLD = 40.0;
constexpr double BASE_LINE_MIN_LENGTH = 10.0;
constexpr double BASE_LINE_MAX_GAP = 5.0;
constexpr double BASE_LINE_MERGE_DISTANCE_THRESHOLD = 5.0;
constexpr double BASE_LINE_MERGE_MAX_GAP = 5.0;
constexpr double BASE_CIRCLE_MIN_DIST = 24.0;
constexpr double BASE_CIRCLE_PARAM1 = 100.0;
constexpr double BASE_GRAY_PARAM2 = 20.0;
constexpr double BASE_THRESHOLD_PARAM2 = 10.0;
constexpr double BASE_MIN_RADIUS = 12.0;
constexpr double BASE_MAX_RADIUS = 16.0;
constexpr double BASE_ANCHOR_INSIDE_TOLERANCE = 6.0;
constexpr double BASE_ANCHOR_OUTSIDE_TOLERANCE = 4.0;
constexpr double BASE_ANCHOR_MIN_SEPARATION = 4.0;
constexpr double BASE_MIN_CENTER_SEGMENT_EXTRA = 2.0;
constexpr double BASE_BORDER_HORIZONTAL_LENGTH = 56.0;
constexpr double BASE_BORDER_DIAGONAL_LENGTH = 52.0;

constexpr double UNIT_SPEED = 1.0;
constexpr double MAX_SIM_TIME_FACTOR = 4.0;
constexpr int MAX_EVENTS_PER_BALL = 10;
constexpr int MAX_TOTAL_EVENTS = 80;
constexpr double POCKET_CAPTURE_FACTOR = 1.7;
constexpr bool USE_DETECTED_FIRST_COLLISION = true;

constexpr double THRESHOLD_VALUE = 250.0;
constexpr double LINE_MERGE_ANGLE_THRESHOLD = 1.0;

constexpr double EPS = 1e-9;
constexpr double EPS_T = 1e-7;
constexpr double POS_EPS = 1e-4;

struct CirclePoint {
    int x = 0;
    int y = 0;
};

struct Segment {
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
    double length = 0.0;
    double A = 0.0;
    double B = 0.0;
    double C = 0.0;
    double midX = 0.0;
    double midY = 0.0;
    double sortTMin = 0.0;
    double sortTMax = 0.0;
};

struct Group {
    std::vector<Segment> segments;
    double sumA = 0.0;
    double sumB = 0.0;
    double sumC = 0.0;
    int count = 0;
};

struct LineInfo {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    std::string category;
    std::vector<int> magentaHits;
    std::vector<int> greenHits;
    std::unordered_map<int, int> magentaAnchors;
    std::unordered_map<int, int> greenAnchors;
};

struct CorrectedLine {
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
    std::string category;
};

struct Pocket {
    double x = 0.0;
    double y = 0.0;
    std::string type;
    int seg = 0;
};

struct SimBall {
    int id = 0;
    std::string role;
    double x = 0.0;
    double y = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    double r = 0.0;
    bool active = true;
    bool pocketed = false;
    double pocketX = 0.0;
    double pocketY = 0.0;
    bool traceLimited = false;
    int cushionHits = 0;
    int ballHits = 0;
    int events = 0;
    int color = 0;
    std::vector<std::pair<double, double>> path;
};

struct CushionEvent {
    double t = 0.0;
    std::string edge;
    int pocket = -1;
};

struct Params {
    double roiScale = 1.0;

    int lineThreshold = 1;
    int lineMinLength = 1;
    int lineMaxGap = 1;

    double lineMergeDistanceThreshold = 1.0;
    double lineMergeMaxGap = 1.0;

    int circleMinDist = 1;
    int grayParam2 = 5;
    int thresholdParam2 = 5;

    int minRadius = 1;
    int maxRadius = 2;
    int fixedRadius = 1;

    int overlapDistance = 1;

    double tangentMinDistance = 1.0;
    double tangentMaxDistance = 2.0;

    double anchorInsideTolerance = 1.0;
    double anchorOutsideTolerance = 1.0;
    double anchorMinSeparation = 1.0;

    double associationMaxDistance = 1.0;
    double minCenterSegmentLength = 1.0;

    int borderHorizontalLength = 1;
    int borderDiagonalLength = 1;
};

bool contains(const std::vector<int>& v, int value) {
    return std::find(v.begin(), v.end(), value) != v.end();
}

std::pair<double, double> normalizeVector(double x, double y) {
    const double n = std::hypot(x, y);

    if (n < 1e-9) {
        return {0.0, 0.0};
    }

    return {x / n, y / n};
}

std::tuple<double, double, double> groupLineFromSegments(
    const std::vector<Segment>& segments
) {
    if (segments.empty()) {
        return {0.0, 0.0, 0.0};
    }

    double sumA = 0.0;
    double sumB = 0.0;

    for (const auto& s : segments) {
        sumA += s.A;
        sumB += s.B;
    }

    auto [avgA, avgB] = normalizeVector(sumA, sumB);

    if (avgA == 0.0 && avgB == 0.0) {
        const auto& s = segments[0];
        return {s.A, s.B, s.C};
    }

    double sumC = 0.0;

    for (const auto& s : segments) {
        sumC += -(avgA * s.midX + avgB * s.midY);
    }

    return {avgA, avgB, sumC / static_cast<double>(segments.size())};
}

std::vector<std::array<int, 4>> mergeLineSegments(
    const std::vector<cv::Vec4i>& lines,
    const Params& params
) {
    std::vector<Segment> segments;

    for (const auto& l : lines) {
        double x1 = static_cast<double>(l[0]);
        double y1 = static_cast<double>(l[1]);
        double x2 = static_cast<double>(l[2]);
        double y2 = static_cast<double>(l[3]);

        double dx = x2 - x1;
        double dy = y2 - y1;
        double length = std::hypot(dx, dy);

        if (length < 1e-6) {
            continue;
        }

        double A = -dy / length;
        double B = dx / length;
        double C = (dy * x1 - dx * y1) / length;

        if (A < 0.0 || (std::abs(A) < 1e-9 && B < 0.0)) {
            A = -A;
            B = -B;
            C = -C;
        }

        Segment seg;
        seg.x1 = x1;
        seg.y1 = y1;
        seg.x2 = x2;
        seg.y2 = y2;
        seg.length = length;
        seg.A = A;
        seg.B = B;
        seg.C = C;
        seg.midX = (x1 + x2) / 2.0;
        seg.midY = (y1 + y2) / 2.0;

        segments.push_back(seg);
    }

    if (segments.empty()) {
        return {};
    }

    std::vector<Group> groups;

    for (const auto& seg : segments) {
        int bestGroupIndex = -1;
        double bestScore = std::numeric_limits<double>::max();

        for (size_t idx = 0; idx < groups.size(); ++idx) {
            const auto& group = groups[idx];

            auto [groupA, groupB] = normalizeVector(group.sumA, group.sumB);

            if (groupA == 0.0 && groupB == 0.0) {
                continue;
            }

            double groupC = group.sumC / static_cast<double>(group.count);

            double dot = std::abs(seg.A * groupA + seg.B * groupB);
            dot = std::max(0.0, std::min(1.0, dot));

            double angleDiff = std::acos(dot) * 180.0 / CV_PI;
            double distance = std::abs(groupA * seg.midX + groupB * seg.midY + groupC);

            if (
                angleDiff <= LINE_MERGE_ANGLE_THRESHOLD &&
                distance <= params.lineMergeDistanceThreshold
            ) {
                double score = angleDiff + distance;

                if (score < bestScore) {
                    bestScore = score;
                    bestGroupIndex = static_cast<int>(idx);
                }
            }
        }

        if (bestGroupIndex < 0) {
            Group g;
            g.segments.push_back(seg);
            g.sumA = seg.A;
            g.sumB = seg.B;
            g.sumC = seg.C;
            g.count = 1;
            groups.push_back(g);
        } else {
            auto& g = groups[bestGroupIndex];
            g.segments.push_back(seg);
            g.sumA += seg.A;
            g.sumB += seg.B;
            g.sumC += seg.C;
            g.count += 1;
        }
    }

    auto makeLineFromCluster =
        [](const std::vector<Segment>& cluster) -> std::optional<std::array<int, 4>> {
            if (cluster.empty()) {
                return std::nullopt;
            }

            if (cluster.size() == 1) {
                const auto& s = cluster[0];

                if (s.length < 1e-6) {
                    return std::nullopt;
                }

                return std::array<int, 4>{
                    static_cast<int>(std::lround(s.x1)),
                    static_cast<int>(std::lround(s.y1)),
                    static_cast<int>(std::lround(s.x2)),
                    static_cast<int>(std::lround(s.y2))
                };
            }

            auto [cA, cB, cC] = groupLineFromSegments(cluster);

            if (cA == 0.0 && cB == 0.0) {
                return std::nullopt;
            }

            double dX = -cB;
            double dY = cA;

            std::vector<double> tValues;

            for (const auto& s : cluster) {
                tValues.push_back(s.x1 * dX + s.y1 * dY);
                tValues.push_back(s.x2 * dX + s.y2 * dY);
            }

            double tStart = *std::min_element(tValues.begin(), tValues.end());
            double tEnd = *std::max_element(tValues.begin(), tValues.end());

            if (tEnd - tStart < 1e-6) {
                return std::nullopt;
            }

            double baseX = -cC * cA;
            double baseY = -cC * cB;

            return std::array<int, 4>{
                static_cast<int>(std::lround(baseX + dX * tStart)),
                static_cast<int>(std::lround(baseY + dY * tStart)),
                static_cast<int>(std::lround(baseX + dX * tEnd)),
                static_cast<int>(std::lround(baseY + dY * tEnd))
            };
        };

    std::vector<std::array<int, 4>> mergedLines;

    for (auto& group : groups) {
        auto& groupSegments = group.segments;

        if (groupSegments.empty()) {
            continue;
        }

        auto [gA, gB, gC] = groupLineFromSegments(groupSegments);

        if (gA == 0.0 && gB == 0.0) {
            continue;
        }

        double directionX = -gB;
        double directionY = gA;

        for (auto& s : groupSegments) {
            double t1 = s.x1 * directionX + s.y1 * directionY;
            double t2 = s.x2 * directionX + s.y2 * directionY;

            s.sortTMin = std::min(t1, t2);
            s.sortTMax = std::max(t1, t2);
        }

        std::sort(
            groupSegments.begin(),
            groupSegments.end(),
            [](const Segment& a, const Segment& b) {
                return a.sortTMin < b.sortTMin;
            }
        );

        std::vector<Segment> currentCluster;
        currentCluster.push_back(groupSegments[0]);

        double currentClusterEnd = groupSegments[0].sortTMax;

        for (size_t i = 1; i < groupSegments.size(); ++i) {
            const auto& s = groupSegments[i];

            if (s.sortTMin <= currentClusterEnd + params.lineMergeMaxGap) {
                if (s.sortTMax > currentClusterEnd) {
                    currentClusterEnd = s.sortTMax;
                }

                currentCluster.push_back(s);
            } else {
                auto line = makeLineFromCluster(currentCluster);

                if (line.has_value()) {
                    mergedLines.push_back(line.value());
                }

                currentCluster.clear();
                currentCluster.push_back(s);
                currentClusterEnd = s.sortTMax;
            }
        }

        auto line = makeLineFromCluster(currentCluster);

        if (line.has_value()) {
            mergedLines.push_back(line.value());
        }
    }

    return mergedLines;
}

std::optional<int> getAnchorEndpointIndex(
    double x1,
    double y1,
    double x2,
    double y2,
    double cx,
    double cy,
    double r,
    const Params& params
) {
    double d1 = std::hypot(x1 - cx, y1 - cy);
    double d2 = std::hypot(x2 - cx, y2 - cy);

    double outsideLimit = std::max(0.0, r - params.anchorOutsideTolerance);

    bool cond1 = (
        d1 <= r + params.anchorInsideTolerance &&
        d1 <= d2 &&
        (d2 >= outsideLimit || (d2 - d1) >= params.anchorMinSeparation)
    );

    bool cond2 = (
        d2 <= r + params.anchorInsideTolerance &&
        d2 <= d1 &&
        (d1 >= outsideLimit || (d1 - d2) >= params.anchorMinSeparation)
    );

    if (cond1 && cond2) {
        return d1 <= d2 ? 0 : 1;
    }

    if (cond1) {
        return 0;
    }

    if (cond2) {
        return 1;
    }

    return std::nullopt;
}

double minEndpointDistanceToPoint(
    const LineInfo& info,
    double px,
    double py
) {
    double d1 = std::hypot(info.x1 - px, info.y1 - py);
    double d2 = std::hypot(info.x2 - px, info.y2 - py);

    return std::min(d1, d2);
}

bool greenTangentToMagenta(
    int greenIdx,
    int magentaIdx,
    const std::vector<CirclePoint>& greenCircles,
    const std::vector<CirclePoint>& magentaCircles,
    const Params& params
) {
    if (
        greenIdx < 0 ||
        greenIdx >= static_cast<int>(greenCircles.size()) ||
        magentaIdx < 0 ||
        magentaIdx >= static_cast<int>(magentaCircles.size())
    ) {
        return false;
    }

    const auto& g = greenCircles[greenIdx];
    const auto& m = magentaCircles[magentaIdx];

    double d = std::hypot(g.x - m.x, g.y - m.y);

    return d >= params.tangentMinDistance && d <= params.tangentMaxDistance;
}

std::array<double, 4> getAnchorAndFreeEndpoints(
    const LineInfo& info,
    bool magenta,
    int circleIdx,
    double circleX,
    double circleY
) {
    const auto& anchors = magenta ? info.magentaAnchors : info.greenAnchors;

    int anchorIdx = -1;

    auto it = anchors.find(circleIdx);

    if (it == anchors.end()) {
        double d1 = std::hypot(info.x1 - circleX, info.y1 - circleY);
        double d2 = std::hypot(info.x2 - circleX, info.y2 - circleY);

        anchorIdx = (d1 <= d2) ? 0 : 1;
    } else {
        anchorIdx = it->second;
    }

    if (anchorIdx == 0) {
        return {
            static_cast<double>(info.x1),
            static_cast<double>(info.y1),
            static_cast<double>(info.x2),
            static_cast<double>(info.y2)
        };
    }

    return {
        static_cast<double>(info.x2),
        static_cast<double>(info.y2),
        static_cast<double>(info.x1),
        static_cast<double>(info.y1)
    };
}

std::pair<double, double> snapCenterToBoundary(
    double cx,
    double cy,
    double radius,
    int roiW,
    int roiH
) {
    double maxX = static_cast<double>(std::max(0, roiW - 1));
    double maxY = static_cast<double>(std::max(0, roiH - 1));

    double minX = std::min(radius, maxX * 0.5);
    double maxXCenter = std::max(maxX - radius, maxX * 0.5);

    double minY = std::min(radius, maxY * 0.5);
    double maxYCenter = std::max(maxY - radius, maxY * 0.5);

    if (minX > maxXCenter) {
        minX = maxXCenter = maxX * 0.5;
    }

    if (minY > maxYCenter) {
        minY = maxYCenter = maxY * 0.5;
    }

    double cxClamped = std::max(minX, std::min(cx, maxXCenter));
    double cyClamped = std::max(minY, std::min(cy, maxYCenter));

    std::vector<std::pair<double, double>> candidates = {
        {minX, cyClamped},
        {maxXCenter, cyClamped},
        {cxClamped, minY},
        {cxClamped, maxYCenter},
        {minX, minY},
        {maxXCenter, minY},
        {minX, maxYCenter},
        {maxXCenter, maxYCenter}
    };

    return *std::min_element(
        candidates.begin(),
        candidates.end(),
        [&](const auto& a, const auto& b) {
            double da = std::hypot(a.first - cx, a.second - cy);
            double db = std::hypot(b.first - cx, b.second - cy);
            return da < db;
        }
    );
}

std::optional<std::pair<double, double>> unitVec(double x, double y) {
    double n = std::hypot(x, y);

    if (n < 1e-9) {
        return std::nullopt;
    }

    return std::make_pair(x / n, y / n);
}

int bgrToColor(int b, int g, int r) {
    return static_cast<int>(0xFF000000u | (r << 16) | (g << 8) | b);
}

std::optional<double> quadSmallestPositive(double a, double b, double c) {
    if (std::abs(a) < EPS) {
        return std::nullopt;
    }

    double disc = b * b - 4.0 * a * c;

    if (disc < 0.0) {
        return std::nullopt;
    }

    double sq = std::sqrt(disc);

    double t1 = (-b - sq) / (2.0 * a);
    double t2 = (-b + sq) / (2.0 * a);

    std::vector<double> candidates;

    if (t1 > EPS_T) candidates.push_back(t1);
    if (t2 > EPS_T) candidates.push_back(t2);

    if (candidates.empty()) {
        return std::nullopt;
    }

    return *std::min_element(candidates.begin(), candidates.end());
}

std::string predictJson(const cv::Mat& bgr, int frameWidth, int frameHeight) {
    double scaleX = static_cast<double>(frameWidth) / static_cast<double>(REFERENCE_WIDTH);
    double scaleY = static_cast<double>(frameHeight) / static_cast<double>(REFERENCE_HEIGHT);

    int roiX = static_cast<int>(std::lround(REFERENCE_ROI_X * scaleX));
    int roiY = static_cast<int>(std::lround(REFERENCE_ROI_Y * scaleY));
    int roiW = static_cast<int>(std::lround(REFERENCE_ROI_W * scaleX));
    int roiH = static_cast<int>(std::lround(REFERENCE_ROI_H * scaleY));

    roiX = std::max(0, std::min(roiX, frameWidth - 1));
    roiY = std::max(0, std::min(roiY, frameHeight - 1));
    roiW = std::max(1, std::min(roiW, frameWidth - roiX));
    roiH = std::max(1, std::min(roiH, frameHeight - roiY));

    if (roiW < 2 || roiH < 2) {
        return "{}";
    }

    cv::Rect roi(roiX, roiY, roiW, roiH);
    cv::Mat roiMat = bgr(roi);

    Params params;

    params.roiScale = std::sqrt(
        static_cast<double>(roiW) * static_cast<double>(roiH) /
        (static_cast<double>(REFERENCE_ROI_W) * static_cast<double>(REFERENCE_ROI_H))
    );

    params.roiScale = std::max(params.roiScale, 1e-6);

    auto scaledInt = [&](double value, int minimum = 1) -> int {
        return std::max(minimum, static_cast<int>(std::lround(value * params.roiScale)));
    };

    auto scaledFloat = [&](double value) -> double {
        return value * params.roiScale;
    };

    params.lineThreshold = std::max(5, scaledInt(BASE_LINE_THRESHOLD));
    params.lineMinLength = scaledInt(BASE_LINE_MIN_LENGTH);
    params.lineMaxGap = scaledInt(BASE_LINE_MAX_GAP);

    params.lineMergeDistanceThreshold = scaledFloat(BASE_LINE_MERGE_DISTANCE_THRESHOLD);
    params.lineMergeMaxGap = scaledFloat(BASE_LINE_MERGE_MAX_GAP);

    params.circleMinDist = scaledInt(BASE_CIRCLE_MIN_DIST);
    params.grayParam2 = std::max(5, static_cast<int>(std::lround(BASE_GRAY_PARAM2 * params.roiScale)));
    params.thresholdParam2 = std::max(5, static_cast<int>(std::lround(BASE_THRESHOLD_PARAM2 * params.roiScale)));

    params.minRadius = scaledInt(BASE_MIN_RADIUS);
    params.maxRadius = std::max(params.minRadius + 1, scaledInt(BASE_MAX_RADIUS, params.minRadius + 1));

    params.fixedRadius = std::max(1, (params.minRadius + params.maxRadius) / 2);

    params.overlapDistance = params.fixedRadius;

    params.tangentMinDistance = 2.0 * params.minRadius;
    params.tangentMaxDistance = 2.0 * params.maxRadius;

    params.anchorInsideTolerance = scaledFloat(BASE_ANCHOR_INSIDE_TOLERANCE);
    params.anchorOutsideTolerance = scaledFloat(BASE_ANCHOR_OUTSIDE_TOLERANCE);
    params.anchorMinSeparation = scaledFloat(BASE_ANCHOR_MIN_SEPARATION);

    params.associationMaxDistance = 3.0 * params.fixedRadius;
    params.minCenterSegmentLength = params.fixedRadius + scaledFloat(BASE_MIN_CENTER_SEGMENT_EXTRA);

    params.borderHorizontalLength = std::min(scaledInt(BASE_BORDER_HORIZONTAL_LENGTH), roiW - 1);
    params.borderDiagonalLength = std::min({
           scaledInt(BASE_BORDER_DIAGONAL_LENGTH),
           roiW - 1,
           roiH - 1
       });

    cv::Mat gray;
    cv::cvtColor(roiMat, gray, cv::COLOR_BGR2GRAY);

    cv::Mat binary;
    cv::threshold(gray, binary, THRESHOLD_VALUE, 255.0, cv::THRESH_BINARY);

    std::vector<cv::Vec4i> lines;
    cv::HoughLinesP(
        binary,
        lines,
        1.0,
        CV_PI / 180.0,
        params.lineThreshold,
        static_cast<double>(params.lineMinLength),
        static_cast<double>(params.lineMaxGap)
    );

    auto mergedLines = mergeLineSegments(lines, params);

    std::vector<cv::Vec3f> grayCirclesRaw;
    cv::HoughCircles(
        gray,
        grayCirclesRaw,
        cv::HOUGH_GRADIENT,
        1.0,
        static_cast<double>(params.circleMinDist),
        BASE_CIRCLE_PARAM1,
        static_cast<double>(params.grayParam2),
        params.minRadius,
        params.maxRadius
    );

    std::vector<cv::Vec3f> thresholdCirclesRaw;
    cv::HoughCircles(
        binary,
        thresholdCirclesRaw,
        cv::HOUGH_GRADIENT,
        1.0,
        static_cast<double>(params.circleMinDist),
        BASE_CIRCLE_PARAM1,
        static_cast<double>(params.thresholdParam2),
        params.minRadius,
        params.maxRadius
    );

    std::vector<CirclePoint> magentaCircles;
    for (const auto& c : thresholdCirclesRaw) {
        magentaCircles.push_back({static_cast<int>(std::lround(c[0])), static_cast<int>(std::lround(c[1]))});
    }

    std::vector<CirclePoint> greenCircles;

    for (const auto& c : grayCirclesRaw) {
        int gx = static_cast<int>(std::lround(c[0]));
        int gy = static_cast<int>(std::lround(c[1]));

        bool overlaps = false;

        for (const auto& t : magentaCircles) {
            if (std::hypot(gx - t.x, gy - t.y) < params.overlapDistance) {
                overlaps = true;
                break;
            }
        }

        if (!overlaps) {
            greenCircles.push_back({gx, gy});
        }
    }

    std::vector<LineInfo> lineInfos;

    for (const auto& line : mergedLines) {
        LineInfo info;
        info.x1 = line[0];
        info.y1 = line[1];
        info.x2 = line[2];
        info.y2 = line[3];

        for (int mi = 0; mi < static_cast<int>(magentaCircles.size()); ++mi) {
            auto anchor = getAnchorEndpointIndex(
                info.x1,
                info.y1,
                info.x2,
                info.y2,
                magentaCircles[mi].x,
                magentaCircles[mi].y,
                params.fixedRadius,
                params
            );

            if (anchor.has_value()) {
                info.magentaHits.push_back(mi);
                info.magentaAnchors[mi] = anchor.value();
            }
        }

        for (int gi = 0; gi < static_cast<int>(greenCircles.size()); ++gi) {
            auto anchor = getAnchorEndpointIndex(
                info.x1,
                info.y1,
                info.x2,
                info.y2,
                greenCircles[gi].x,
                greenCircles[gi].y,
                params.fixedRadius,
                params
            );

            if (anchor.has_value()) {
                info.greenHits.push_back(gi);
                info.greenAnchors[gi] = anchor.value();
            }
        }

        if (!info.magentaHits.empty() && !info.greenHits.empty()) {
            info.category = "both";
        } else if (!info.magentaHits.empty()) {
            info.category = "magenta_only";
        } else if (!info.greenHits.empty()) {
            info.category = "green_only";
        } else {
            info.category = "normal";
        }

        lineInfos.push_back(info);
    }

    int selectedMagenta = -1;

    if (!magentaCircles.empty()) {
        std::vector<int> magentaScores(magentaCircles.size(), 0);

        for (const auto& info : lineInfos) {
            if (info.category == "both" || info.category == "magenta_only") {
                for (int mi : info.magentaHits) {
                    magentaScores[mi] += (info.category == "both") ? 2 : 1;
                }
            }
        }

        int bestScore = *std::max_element(magentaScores.begin(), magentaScores.end());

        if (bestScore > 0) {
            selectedMagenta = static_cast<int>(
                std::find(magentaScores.begin(), magentaScores.end(), bestScore) - magentaScores.begin()
            );
        } else {
            selectedMagenta = 0;
        }
    }

    int selectedYellow = -1;
    int selectedCyan = -1;

    std::vector<int> bothCandidates;

    for (int i = 0; i < static_cast<int>(lineInfos.size()); ++i) {
        if (lineInfos[i].category == "both") {
            bothCandidates.push_back(i);
        }
    }

    if (!bothCandidates.empty()) {
        double bestYellowScore = std::numeric_limits<double>::max();

        for (int idx : bothCandidates) {
            const auto& info = lineInfos[idx];

            double magentaBonus = 1000.0;

            if (selectedMagenta >= 0 && contains(info.magentaHits, selectedMagenta)) {
                magentaBonus = 0.0;
            }

            for (int gi : info.greenHits) {
                double tangentPenalty = 0.0;

                if (
                    selectedMagenta >= 0 &&
                    greenTangentToMagenta(gi, selectedMagenta, greenCircles, magentaCircles, params)
                ) {
                    tangentPenalty = 100.0;
                }

                double dGreen = minEndpointDistanceToPoint(
                    info,
                    greenCircles[gi].x,
                    greenCircles[gi].y
                );

                double dMagenta = 0.0;

                if (selectedMagenta >= 0) {
                    dMagenta = minEndpointDistanceToPoint(
                        info,
                        magentaCircles[selectedMagenta].x,
                        magentaCircles[selectedMagenta].y
                    );
                } else if (!info.magentaHits.empty()) {
                    dMagenta = std::numeric_limits<double>::max();

                    for (int mi : info.magentaHits) {
                        dMagenta = std::min(
                            dMagenta,
                            minEndpointDistanceToPoint(
                                info,
                                magentaCircles[mi].x,
                                magentaCircles[mi].y
                            )
                        );
                    }
                }

                double score = magentaBonus + tangentPenalty + dMagenta + dGreen;

                if (score < bestYellowScore) {
                    bestYellowScore = score;
                    selectedYellow = idx;
                    selectedCyan = gi;
                }
            }
        }
    }

    if (selectedYellow >= 0 && !lineInfos[selectedYellow].magentaHits.empty()) {
        if (selectedMagenta < 0) {
            selectedMagenta = lineInfos[selectedYellow].magentaHits[0];
        } else if (!contains(lineInfos[selectedYellow].magentaHits, selectedMagenta)) {
            int bestMagenta = -1;
            double bestDistance = std::numeric_limits<double>::max();

            for (int mi : lineInfos[selectedYellow].magentaHits) {
                double d = minEndpointDistanceToPoint(
                    lineInfos[selectedYellow],
                    magentaCircles[mi].x,
                    magentaCircles[mi].y
                );

                if (d < bestDistance) {
                    bestDistance = d;
                    bestMagenta = mi;
                }
            }

            selectedMagenta = bestMagenta;
        }
    }

    int selectedOrange = -1;

    if (!greenCircles.empty()) {
        std::vector<int> orangeCandidates;

        if (selectedMagenta >= 0) {
            for (int gi = 0; gi < static_cast<int>(greenCircles.size()); ++gi) {
                if (
                    gi != selectedCyan &&
                    greenTangentToMagenta(gi, selectedMagenta, greenCircles, magentaCircles, params)
                ) {
                    orangeCandidates.push_back(gi);
                }
            }
        }

        if (orangeCandidates.empty()) {
            for (int gi = 0; gi < static_cast<int>(greenCircles.size()); ++gi) {
                if (gi == selectedCyan) {
                    continue;
                }

                bool tangentToAny = false;

                for (int mi = 0; mi < static_cast<int>(magentaCircles.size()); ++mi) {
                    if (greenTangentToMagenta(gi, mi, greenCircles, magentaCircles, params)) {
                        tangentToAny = true;
                        break;
                    }
                }

                if (tangentToAny) {
                    orangeCandidates.push_back(gi);
                }
            }
        }

        if (!orangeCandidates.empty()) {
            double bestOrangeScore = std::numeric_limits<double>::max();

            for (int gi : orangeCandidates) {
                double score = 0.0;

                std::vector<int> greenOnlyHits;

                for (int i = 0; i < static_cast<int>(lineInfos.size()); ++i) {
                    if (
                        lineInfos[i].category == "green_only" &&
                        contains(lineInfos[i].greenHits, gi)
                    ) {
                        greenOnlyHits.push_back(i);
                    }
                }

                if (!greenOnlyHits.empty()) {
                    double best = std::numeric_limits<double>::max();

                    for (int idx : greenOnlyHits) {
                        best = std::min(
                            best,
                            minEndpointDistanceToPoint(
                                lineInfos[idx],
                                greenCircles[gi].x,
                                greenCircles[gi].y
                            )
                        );
                    }

                    score += best;
                } else {
                    score += 1000.0;
                }

                if (selectedMagenta >= 0) {
                    double dToMagenta = std::hypot(
                        greenCircles[gi].x - magentaCircles[selectedMagenta].x,
                        greenCircles[gi].y - magentaCircles[selectedMagenta].y
                    );

                    score += std::abs(dToMagenta - 2.0 * params.fixedRadius);
                }

                if (score < bestOrangeScore) {
                    bestOrangeScore = score;
                    selectedOrange = gi;
                }
            }
        }
    }

    int selectedRed = -1;

    if (selectedMagenta >= 0) {
        std::vector<int> redCandidates;
        std::vector<int> redHitCandidates;

        for (int i = 0; i < static_cast<int>(lineInfos.size()); ++i) {
            if (lineInfos[i].category != "magenta_only") {
                continue;
            }

            redCandidates.push_back(i);

            if (contains(lineInfos[i].magentaHits, selectedMagenta)) {
                redHitCandidates.push_back(i);
            }
        }

        const auto& pool = redHitCandidates.empty() ? redCandidates : redHitCandidates;

        if (!pool.empty()) {
            double bestRedDistance = std::numeric_limits<double>::max();
            int bestRedCandidate = -1;

            double mx = magentaCircles[selectedMagenta].x;
            double my = magentaCircles[selectedMagenta].y;

            for (int idx : pool) {
                double d = minEndpointDistanceToPoint(lineInfos[idx], mx, my);

                if (d < bestRedDistance) {
                    bestRedDistance = d;
                    bestRedCandidate = idx;
                }
            }

            if (bestRedCandidate >= 0) {
                if (!redHitCandidates.empty() || bestRedDistance <= params.associationMaxDistance) {
                    selectedRed = bestRedCandidate;
                }
            }
        }
    }

    int selectedBlue = -1;

    if (selectedOrange >= 0) {
        std::vector<int> blueCandidates;
        std::vector<int> blueHitCandidates;

        for (int i = 0; i < static_cast<int>(lineInfos.size()); ++i) {
            if (lineInfos[i].category != "green_only") {
                continue;
            }

            blueCandidates.push_back(i);

            if (contains(lineInfos[i].greenHits, selectedOrange)) {
                blueHitCandidates.push_back(i);
            }
        }

        const auto& pool = blueHitCandidates.empty() ? blueCandidates : blueHitCandidates;

        if (!pool.empty()) {
            double bestBlueDistance = std::numeric_limits<double>::max();
            int bestBlueCandidate = -1;

            double ox = greenCircles[selectedOrange].x;
            double oy = greenCircles[selectedOrange].y;

            for (int idx : pool) {
                double d = minEndpointDistanceToPoint(lineInfos[idx], ox, oy);

                if (d < bestBlueDistance) {
                    bestBlueDistance = d;
                    bestBlueCandidate = idx;
                }
            }

            if (bestBlueCandidate >= 0) {
                if (!blueHitCandidates.empty() || bestBlueDistance <= params.associationMaxDistance) {
                    selectedBlue = bestBlueCandidate;
                }
            }
        }
    }

    std::vector<CorrectedLine> correctedLines;

    std::optional<std::pair<double, double>> originalMagentaCenter;
    std::optional<std::pair<double, double>> correctedMagentaCenter;

    if (selectedMagenta >= 0) {
        originalMagentaCenter = {
            static_cast<double>(magentaCircles[selectedMagenta].x),
            static_cast<double>(magentaCircles[selectedMagenta].y)
        };

        correctedMagentaCenter = originalMagentaCenter;
    }

    std::optional<std::pair<double, double>> fixedBlueAnchor;
    std::optional<double> fixedBlueAngle;
    std::optional<double> fixedBlueLength;

    if (selectedBlue >= 0 && selectedOrange >= 0) {
        double ox = greenCircles[selectedOrange].x;
        double oy = greenCircles[selectedOrange].y;

        auto endpoints = getAnchorAndFreeEndpoints(
            lineInfos[selectedBlue],
            false,
            selectedOrange,
            ox,
            oy
        );

        double ax = endpoints[0];
        double ay = endpoints[1];
        double fx = endpoints[2];
        double fy = endpoints[3];

        double vx = fx - ox;
        double vy = fy - oy;
        double length = std::hypot(vx, vy);

        double originalLength = std::hypot(fx - ax, fy - ay);

        if (length < 1e-6) {
            vx = fx - ax;
            vy = fy - ay;
            length = std::hypot(vx, vy);
        }

        length = std::max(length, std::max(originalLength, params.minCenterSegmentLength));

        fixedBlueAngle = (length > 1e-6) ? std::atan2(vy, vx) : 0.0;
        fixedBlueLength = length;
        fixedBlueAnchor = {ox, oy};
    }

    if (correctedMagentaCenter.has_value()) {
        double mx = correctedMagentaCenter->first;
        double my = correctedMagentaCenter->second;

        if (selectedOrange >= 0) {
            double ox = greenCircles[selectedOrange].x;
            double oy = greenCircles[selectedOrange].y;

            double tangentDistance = 2.0 * params.fixedRadius;

            if (fixedBlueAngle.has_value()) {
                double ux = std::cos(fixedBlueAngle.value());
                double uy = std::sin(fixedBlueAngle.value());

                mx = ox - ux * tangentDistance;
                my = oy - uy * tangentDistance;
            } else {
                double vx = mx - ox;
                double vy = my - oy;
                double d = std::hypot(vx, vy);

                if (d < 1e-6) {
                    vx = 1.0;
                    vy = 0.0;
                    d = 1.0;
                }

                mx = ox + (vx / d) * tangentDistance;
                my = oy + (vy / d) * tangentDistance;
            }

            correctedMagentaCenter = {mx, my};
        } else {
            correctedMagentaCenter = snapCenterToBoundary(
                mx,
                my,
                params.fixedRadius,
                roiW,
                roiH
            );
        }
    }

    if (
        selectedYellow >= 0 &&
        selectedCyan >= 0 &&
        correctedMagentaCenter.has_value()
    ) {
        double cx = greenCircles[selectedCyan].x;
        double cy = greenCircles[selectedCyan].y;

        correctedLines.push_back({
            cx,
            cy,
            correctedMagentaCenter->first,
            correctedMagentaCenter->second,
            "both"
        });
    } else if (selectedYellow >= 0) {
        const auto& info = lineInfos[selectedYellow];

        correctedLines.push_back({
            static_cast<double>(info.x1),
            static_cast<double>(info.y1),
            static_cast<double>(info.x2),
            static_cast<double>(info.y2),
            "both"
        });
    }

    if (
        fixedBlueAnchor.has_value() &&
        fixedBlueAngle.has_value() &&
        fixedBlueLength.has_value()
    ) {
        double ox = fixedBlueAnchor->first;
        double oy = fixedBlueAnchor->second;

        double bx = ox + std::cos(fixedBlueAngle.value()) * fixedBlueLength.value();
        double by = oy + std::sin(fixedBlueAngle.value()) * fixedBlueLength.value();

        correctedLines.push_back({
            ox,
            oy,
            bx,
            by,
            "green_only"
        });
    } else if (selectedBlue >= 0) {
        const auto& info = lineInfos[selectedBlue];

        correctedLines.push_back({
            static_cast<double>(info.x1),
            static_cast<double>(info.y1),
            static_cast<double>(info.x2),
            static_cast<double>(info.y2),
            "green_only"
        });
    }

    if (
        selectedRed >= 0 &&
        correctedMagentaCenter.has_value() &&
        selectedMagenta >= 0
    ) {
        double mx = correctedMagentaCenter->first;
        double my = correctedMagentaCenter->second;

        auto anchorReference = originalMagentaCenter.has_value()
                                   ? originalMagentaCenter
                                   : correctedMagentaCenter;

        auto endpoints = getAnchorAndFreeEndpoints(
            lineInfos[selectedRed],
            true,
            selectedMagenta,
            anchorReference->first,
            anchorReference->second
        );

        double ax = endpoints[0];
        double ay = endpoints[1];
        double fx = endpoints[2];
        double fy = endpoints[3];

        double dxSeg = fx - ax;
        double dySeg = fy - ay;
        double segLength = std::hypot(dxSeg, dySeg);

        double preliminaryRedAngle = 0.0;
        double redLength = params.minCenterSegmentLength;

        if (segLength > 1e-6) {
            preliminaryRedAngle = std::atan2(dySeg, dxSeg);
            redLength = std::max(segLength, params.minCenterSegmentLength);
        } else {
            double dxF = fx - mx;
            double dyF = fy - my;
            double fl = std::hypot(dxF, dyF);

            if (fl > 1e-6) {
                preliminaryRedAngle = std::atan2(dyF, dxF);
                redLength = std::max(fl, params.minCenterSegmentLength);
            }
        }

        double redAngle = preliminaryRedAngle;

        if (fixedBlueAngle.has_value()) {
            double perpA = fixedBlueAngle.value() + CV_PI / 2.0;
            double perpB = fixedBlueAngle.value() - CV_PI / 2.0;

            double diffA = std::abs(
                std::atan2(
                    std::sin(perpA - preliminaryRedAngle),
                    std::cos(perpA - preliminaryRedAngle)
                )
            );

            double diffB = std::abs(
                std::atan2(
                    std::sin(perpB - preliminaryRedAngle),
                    std::cos(perpB - preliminaryRedAngle)
                )
            );

            redAngle = (diffA <= diffB) ? perpA : perpB;
        }

        double rx = mx + std::cos(redAngle) * redLength;
        double ry = my + std::sin(redAngle) * redLength;

        correctedLines.push_back({
            mx,
            my,
            rx,
            ry,
            "magenta_only"
        });
    } else if (selectedRed >= 0) {
        const auto& info = lineInfos[selectedRed];

        correctedLines.push_back({
            static_cast<double>(info.x1),
            static_cast<double>(info.y1),
            static_cast<double>(info.x2),
            static_cast<double>(info.y2),
            "magenta_only"
        });
    }

    if (correctedLines.empty()) {
        for (const auto& info : lineInfos) {
            if (
                info.category == "both" ||
                info.category == "magenta_only" ||
                info.category == "green_only"
            ) {
                correctedLines.push_back({
                    static_cast<double>(info.x1),
                    static_cast<double>(info.y1),
                    static_cast<double>(info.x2),
                    static_cast<double>(info.y2),
                    info.category
                });
            }
        }
    }

    std::optional<std::pair<double, double>> yellowDir;
    std::optional<std::pair<double, double>> redDir;
    std::optional<std::pair<double, double>> blueDir;

    for (const auto& info : correctedLines) {
        auto d = unitVec(info.x2 - info.x1, info.y2 - info.y1);

        if (!d.has_value()) {
            continue;
        }

        if (info.category == "both" && !yellowDir.has_value()) {
            yellowDir = d;
        } else if (info.category == "magenta_only" && !redDir.has_value()) {
            redDir = d;
        } else if (info.category == "green_only" && !blueDir.has_value()) {
            blueDir = d;
        }
    }

    int cueColor = bgrToColor(255, 255, 0);
    int targetColor = bgrToColor(0, 165, 255);

    std::vector<int> objectPalette = {
        bgrToColor(255, 0, 255),
        bgrToColor(0, 255, 0),
        bgrToColor(0, 0, 255),
        bgrToColor(255, 0, 0),
        bgrToColor(128, 0, 128),
        bgrToColor(147, 0, 255),
        bgrToColor(200, 200, 0),
        bgrToColor(80, 80, 80)
    };

    std::vector<SimBall> balls;

    int objCounter = 0;

    for (int gi = 0; gi < static_cast<int>(greenCircles.size()); ++gi) {
        SimBall ball;

        ball.id = gi;
        ball.x = greenCircles[gi].x;
        ball.y = greenCircles[gi].y;
        ball.r = params.fixedRadius;
        ball.path.push_back({ball.x, ball.y});

        if (gi == selectedCyan) {
            ball.role = "cue";
            ball.color = cueColor;
        } else if (gi == selectedOrange) {
            ball.role = "target";
            ball.color = targetColor;
        } else {
            ball.role = "object";
            ball.color = objectPalette[objCounter % objectPalette.size()];
            ++objCounter;
        }

        balls.push_back(ball);
    }

    int cueBallIndex = -1;

    for (int i = 0; i < static_cast<int>(balls.size()); ++i) {
        if (balls[i].role == "cue") {
            cueBallIndex = i;
            break;
        }
    }

    std::optional<std::pair<double, double>> cueDir0;

    if (cueBallIndex >= 0) {
        const auto& cueBall = balls[cueBallIndex];

        if (correctedMagentaCenter.has_value()) {
            cueDir0 = unitVec(
                correctedMagentaCenter->first - cueBall.x,
                correctedMagentaCenter->second - cueBall.y
            );
        }

        if (!cueDir0.has_value() && yellowDir.has_value()) {
            cueDir0 = yellowDir;
        }

        if (cueDir0.has_value()) {
            balls[cueBallIndex].vx = cueDir0->first * UNIT_SPEED;
            balls[cueBallIndex].vy = cueDir0->second * UNIT_SPEED;
        }
    }

    double Wm = static_cast<double>(roiW - 1);
    double Hm = static_cast<double>(roiH - 1);
    double cxMid = Wm / 2.0;

    double cornerMouth = static_cast<double>(params.borderDiagonalLength) / std::sqrt(2.0);
    double sideMouth = static_cast<double>(params.borderHorizontalLength) / 2.0;
    double capR = POCKET_CAPTURE_FACTOR * static_cast<double>(params.fixedRadius);

    std::vector<Pocket> pockets = {
        {0.0, 0.0, "corner", 0},
        {Wm, 0.0, "corner", 1},
        {0.0, Hm, "corner", 2},
        {Wm, Hm, "corner", 3},
        {cxMid, 0.0, "side", 4},
        {cxMid, Hm, "side", 5}
    };

    auto mouthPocketFor = [&](const std::string& edge, double u) -> int {
        if (edge == "left") {
            return (u < cornerMouth) ? 0 : 2;
        }

        if (edge == "right") {
            return (u < cornerMouth) ? 1 : 3;
        }

        if (edge == "top") {
            if (u < cornerMouth) return 0;
            if (u > Wm - cornerMouth) return 1;
            return 4;
        }

        if (u < cornerMouth) return 2;
        if (u > Wm - cornerMouth) return 3;

        return 5;
    };

    auto inPocketMouth = [&](const std::string& edge, double u) -> bool {
        if (edge == "left" || edge == "right") {
            return u < cornerMouth || u > Hm - cornerMouth;
        }

        return u < cornerMouth || u > Wm - cornerMouth || std::abs(u - cxMid) < sideMouth;
    };

    auto ballSpeed = [](const SimBall& b) -> double {
        return std::hypot(b.vx, b.vy);
    };

    auto captureEvent = [&](const SimBall& b) -> std::optional<std::pair<double, int>> {
        double a = b.vx * b.vx + b.vy * b.vy;

        for (int pi = 0; pi < static_cast<int>(pockets.size()); ++pi) {
            double dpx = b.x - pockets[pi].x;
            double dpy = b.y - pockets[pi].y;

            if (dpx * dpx + dpy * dpy - capR * capR <= 0.0) {
                return std::make_pair(0.0, pi);
            }
        }

        if (a < EPS) {
            return std::nullopt;
        }

        std::optional<double> best;
        int bestPocket = -1;

        for (int pi = 0; pi < static_cast<int>(pockets.size()); ++pi) {
            double dpx = b.x - pockets[pi].x;
            double dpy = b.y - pockets[pi].y;

            double c0 = dpx * dpx + dpy * dpy - capR * capR;
            double bb = 2.0 * (b.vx * dpx + b.vy * dpy);

            if (bb >= 0.0) {
                continue;
            }

            auto t = quadSmallestPositive(a, bb, c0);

            if (t.has_value() && (!best.has_value() || t.value() < best.value())) {
                best = t;
                bestPocket = pi;
            }
        }

        if (best.has_value()) {
            return std::make_pair(best.value(), bestPocket);
        }

        return std::nullopt;
    };

    auto cushionEvent = [&](const SimBall& b) -> std::optional<CushionEvent> {
        double r = b.r;
        double px = b.x;
        double py = b.y;
        double vx = b.vx;
        double vy = b.vy;

        std::optional<CushionEvent> best;

        struct Spec {
            std::string edge;
            double vel;
            double pos;
            double lim;
            int sign;
        };

        std::vector<Spec> specs = {
            {"left", vx, px, r, -1},
            {"right", vx, px, Wm - r, +1},
            {"top", vy, py, r, -1},
            {"bottom", vy, py, Hm - r, +1}
        };

        for (const auto& spec : specs) {
            if (spec.sign < 0) {
                if (spec.vel >= -EPS) continue;
            } else {
                if (spec.vel <= EPS) continue;
            }

            double t = (spec.lim - spec.pos) / spec.vel;

            if (t < 0.0) continue;

            double u = (spec.edge == "left" || spec.edge == "right")
                           ? py + vy * t
                           : px + vx * t;

            CushionEvent candidate;
            candidate.t = t;
            candidate.edge = spec.edge;

            if (inPocketMouth(spec.edge, u)) {
                candidate.pocket = mouthPocketFor(spec.edge, u);
            } else {
                candidate.pocket = -1;
            }

            if (!best.has_value() || candidate.t < best->t) {
                best = candidate;
            }
        }

        return best;
    };

    auto ballBallTime = [&](const SimBall& bi, const SimBall& bj) -> std::optional<double> {
        double dpx = bj.x - bi.x;
        double dpy = bj.y - bi.y;

        double dvx = bj.vx - bi.vx;
        double dvy = bj.vy - bi.vy;

        double R = bi.r + bj.r;

        double a = dvx * dvx + dvy * dvy;
        double b = 2.0 * (dpx * dvx + dpy * dvy);
        double c = dpx * dpx + dpy * dpy - R * R;

        if (c <= 0.0) {
            if (b < 0.0) {
                return 0.0;
            }

            return std::nullopt;
        }

        auto t = quadSmallestPositive(a, b, c);

        if (!t.has_value()) {
            return std::nullopt;
        }

        double cx = dpx + dvx * t.value();
        double cy = dpy + dvy * t.value();

        if (cx * dvx + cy * dvy >= 0.0) {
            return std::nullopt;
        }

        return t;
    };

    auto pocketBall = [&](SimBall& b, int pocketIndex) {
        const auto& pk = pockets[pocketIndex];

        b.path.push_back({pk.x, pk.y});
        b.pocketed = true;
        b.pocketX = pk.x;
        b.pocketY = pk.y;
        b.active = false;
        b.vx = 0.0;
        b.vy = 0.0;
    };

    auto checkEventBudget = [&](SimBall& b) {
        if (b.events >= MAX_EVENTS_PER_BALL) {
            b.active = false;
            b.traceLimited = true;
            b.vx = 0.0;
            b.vy = 0.0;
        }
    };

    std::vector<std::pair<double, double>> ghostPoints;

    int totalEvents = 0;
    double simTime = 0.0;
    bool firstCollisionDone = false;

    double maxSimTime = MAX_SIM_TIME_FACTOR * (roiW + roiH);

    while (totalEvents < MAX_TOTAL_EVENTS && simTime < maxSimTime) {
        std::vector<int> movers;

        for (int i = 0; i < static_cast<int>(balls.size()); ++i) {
            if (balls[i].active && ballSpeed(balls[i]) > EPS) {
                movers.push_back(i);
            }
        }

        if (movers.empty()) {
            break;
        }

        std::optional<double> bestT;
        std::string bestKind;

        int bestBall = -1;
        int bestPocket = -1;
        std::string bestEdge;

        int bestBi = -1;
        int bestBj = -1;

        for (int idx : movers) {
            auto cap = captureEvent(balls[idx]);

            if (cap.has_value() && (!bestT.has_value() || cap->first < bestT.value())) {
                bestT = cap->first;
                bestKind = "pocket";
                bestBall = idx;
                bestPocket = cap->second;
                bestEdge.clear();
                bestBi = -1;
                bestBj = -1;
            }
        }

        for (int idx : movers) {
            auto c = cushionEvent(balls[idx]);

            if (c.has_value() && (!bestT.has_value() || c->t < bestT.value())) {
                bestT = c->t;

                if (c->pocket >= 0) {
                    bestKind = "mouth";
                    bestPocket = c->pocket;
                } else {
                    bestKind = "cushion";
                    bestPocket = -1;
                }

                bestBall = idx;
                bestEdge = c->edge;
                bestBi = -1;
                bestBj = -1;
            }
        }

        std::vector<int> active;

        for (int i = 0; i < static_cast<int>(balls.size()); ++i) {
            if (balls[i].active) {
                active.push_back(i);
            }
        }

        for (int i = 0; i < static_cast<int>(active.size()); ++i) {
            for (int j = i + 1; j < static_cast<int>(active.size()); ++j) {
                int bi = active[i];
                int bj = active[j];

                if (ballSpeed(balls[bi]) < EPS && ballSpeed(balls[bj]) < EPS) {
                    continue;
                }

                auto t = ballBallTime(balls[bi], balls[bj]);

                if (t.has_value() && (!bestT.has_value() || t.value() < bestT.value())) {
                    bestT = t;
                    bestKind = "balls";
                    bestBi = bi;
                    bestBj = bj;

                    bestBall = -1;
                    bestPocket = -1;
                    bestEdge.clear();
                }
            }
        }

        double remaining = maxSimTime - simTime;

        if (!bestT.has_value() || bestT.value() > remaining) {
            for (int idx : movers) {
                balls[idx].x += balls[idx].vx * remaining;
                balls[idx].y += balls[idx].vy * remaining;
            }

            simTime += remaining;
            break;
        }

        double dt = bestT.value();

        for (auto& b : balls) {
            if (b.active) {
                b.x += b.vx * dt;
                b.y += b.vy * dt;
            }
        }

        simTime += dt;
        ++totalEvents;

        for (auto& b : balls) {
            if (
                b.active &&
                (
                    b.x < -capR ||
                    b.x > Wm + capR ||
                    b.y < -capR ||
                    b.y > Hm + capR
                )
            ) {
                int nearest = 0;
                double bestDistance = std::numeric_limits<double>::max();

                for (int pi = 0; pi < static_cast<int>(pockets.size()); ++pi) {
                    double dx = pockets[pi].x - b.x;
                    double dy = pockets[pi].y - b.y;
                    double d = dx * dx + dy * dy;

                    if (d < bestDistance) {
                        bestDistance = d;
                        nearest = pi;
                    }
                }

                pocketBall(b, nearest);
            }
        }

        if (bestKind == "pocket" || bestKind == "mouth") {
            if (bestBall >= 0 && balls[bestBall].active) {
                pocketBall(balls[bestBall], bestPocket);
            }
        } else if (bestKind == "cushion") {
            if (bestBall >= 0 && balls[bestBall].active) {
                auto& b = balls[bestBall];

                b.path.push_back({b.x, b.y});
                ghostPoints.push_back({b.x, b.y});

                if (bestEdge == "left") {
                    b.vx = std::abs(b.vx);
                    b.x = b.r + POS_EPS;
                } else if (bestEdge == "right") {
                    b.vx = -std::abs(b.vx);
                    b.x = Wm - b.r - POS_EPS;
                } else if (bestEdge == "top") {
                    b.vy = std::abs(b.vy);
                    b.y = b.r + POS_EPS;
                } else {
                    b.vy = -std::abs(b.vy);
                    b.y = Hm - b.r - POS_EPS;
                }

                ++b.cushionHits;
                ++b.events;

                checkEventBudget(b);
            }
        } else if (bestKind == "balls") {
            if (bestBi >= 0 && bestBj >= 0 && balls[bestBi].active && balls[bestBj].active) {
                auto& bi = balls[bestBi];
                auto& bj = balls[bestBj];

                int strikerIndex = (ballSpeed(bi) >= ballSpeed(bj)) ? bestBi : bestBj;

                ghostPoints.push_back({balls[strikerIndex].x, balls[strikerIndex].y});

                double dx = bj.x - bi.x;
                double dy = bj.y - bi.y;
                double d = std::hypot(dx, dy);

                double nx = 1.0;
                double ny = 0.0;

                if (d >= EPS) {
                    nx = dx / d;
                    ny = dy / d;
                }

                bool overridden = false;

                if (
                    USE_DETECTED_FIRST_COLLISION &&
                    !firstCollisionDone &&
                    (
                        (bi.role == "cue" && bj.role == "target") ||
                        (bi.role == "target" && bj.role == "cue")
                    ) &&
                    redDir.has_value() &&
                    blueDir.has_value() &&
                    cueDir0.has_value()
                ) {
                    double sObj = UNIT_SPEED * std::max(
                        0.0,
                        cueDir0->first * blueDir->first +
                        cueDir0->second * blueDir->second
                    );

                    double sCue = UNIT_SPEED * std::max(
                        0.0,
                        cueDir0->first * redDir->first +
                        cueDir0->second * redDir->second
                    );

                    if (sObj > 1e-3) {
                        SimBall& cb = (bi.role == "cue") ? bi : bj;
                        SimBall& tb = (bi.role == "cue") ? bj : bi;

                        cb.vx = redDir->first * sCue;
                        cb.vy = redDir->second * sCue;

                        tb.vx = blueDir->first * sObj;
                        tb.vy = blueDir->second * sObj;

                        overridden = true;
                    }

                    firstCollisionDone = true;
                }

                if (!overridden) {
                    double viN = bi.vx * nx + bi.vy * ny;
                    double vjN = bj.vx * nx + bj.vy * ny;

                    bi.vx += (vjN - viN) * nx;
                    bi.vy += (vjN - viN) * ny;

                    bj.vx += (viN - vjN) * nx;
                    bj.vy += (viN - vjN) * ny;
                }

                double overlap = (bi.r + bj.r) - d;

                if (overlap > 0.0) {
                    double half = overlap / 2.0 + POS_EPS;

                    bi.x -= nx * half;
                    bi.y -= ny * half;

                    bj.x += nx * half;
                    bj.y += ny * half;
                }

                bi.path.push_back({bi.x, bi.y});
                bj.path.push_back({bj.x, bj.y});

                ++bi.ballHits;
                ++bj.ballHits;

                ++bi.events;
                ++bj.events;

                checkEventBudget(bi);
                checkEventBudget(bj);
            }
        }
    }

    for (auto& b : balls) {
        if (b.active && ballSpeed(b) > EPS) {
            b.path.push_back({b.x, b.y});
        }
    }

    std::ostringstream os;

    os << std::fixed << std::setprecision(2);

    os << "{";
    os << "\"screenWidth\":" << frameWidth << ",";
    os << "\"screenHeight\":" << frameHeight << ",";
    os << "\"ballRadius\":" << params.fixedRadius << ",";
    os << "\"roi\":{";
    os << "\"left\":" << roiX << ",";
    os << "\"top\":" << roiY << ",";
    os << "\"right\":" << roiX + roiW << ",";
    os << "\"bottom\":" << roiY + roiH;
    os << "},";

    os << "\"paths\":[";

    for (size_t i = 0; i < balls.size(); ++i) {
        if (i > 0) os << ",";

        const auto& b = balls[i];

        os << "{";
        os << "\"role\":\"" << b.role << "\",";
        os << "\"color\":" << b.color << ",";
        os << "\"radius\":" << params.fixedRadius << ",";
        os << "\"points\":[";

        for (size_t j = 0; j < b.path.size(); ++j) {
            if (j > 0) os << ",";

            os << "["
               << roiX + b.path[j].first << ","
               << roiY + b.path[j].second
               << "]";
        }

        os << "]}";
    }

    os << "],";

    os << "\"ghosts\":[";

    for (size_t i = 0; i < ghostPoints.size(); ++i) {
        if (i > 0) os << ",";

        os << "["
           << roiX + ghostPoints[i].first << ","
           << roiY + ghostPoints[i].second
           << "]";
    }

    os << "],";

    os << "\"pocketMarks\":[";

    bool firstPocketMark = true;

    for (const auto& b : balls) {
        if (!b.pocketed) continue;

        if (!firstPocketMark) os << ",";
        firstPocketMark = false;

        os << "{"
           << "\"x\":" << roiX + b.pocketX << ","
           << "\"y\":" << roiY + b.pocketY << ","
           << "\"color\":" << b.color
           << "}";
    }

    os << "]";

    os << "}";

    return os.str();
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_poolpredictor_NativePredictor_predictJson(
    JNIEnv* env,
    jobject /* this */,
    jbyteArray rgbaBytes,
    jint width,
    jint height
) {
    try {
        if (rgbaBytes == nullptr || width <= 0 || height <= 0) {
            return env->NewStringUTF("{}");
        }

        jsize expectedSize = width * height * 4;
        jsize arraySize = env->GetArrayLength(rgbaBytes);

        if (arraySize < expectedSize) {
            return env->NewStringUTF("{}");
        }

        jbyte* data = env->GetByteArrayElements(rgbaBytes, nullptr);

        if (data == nullptr) {
            return env->NewStringUTF("{}");
        }

        cv::Mat rgba(height, width, CV_8UC4, data);
        cv::Mat bgr;

        cv::cvtColor(rgba, bgr, cv::COLOR_RGBA2BGR);

        std::string json = predictJson(bgr, width, height);

        env->ReleaseByteArrayElements(rgbaBytes, data, JNI_ABORT);

        return env->NewStringUTF(json.c_str());
    } catch (const std::exception& e) {
        LOGE("Predictor exception: %s", e.what());
        return env->NewStringUTF("{}");
    } catch (...) {
        LOGE("Unknown predictor exception");
        return env->NewStringUTF("{}");
    }
}
