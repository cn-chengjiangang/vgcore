﻿// mgsnapimpl.cpp: 实现命令管理器类
// Copyright (c) 2004-2013, Zhang Yungui
// License: LGPL, https://github.com/rhcad/touchvg

#include "mgcmdmgr_.h"
#include "mgbasicsps.h"

class SnapItem {
public:
    Point2d pt;             // 捕捉到的坐标
    Point2d base;           // 参考线基准点、原始点
    Point2d startpt;        // 垂线起始点
    float   maxdist;        // 最大容差
    float   dist;           // 捕捉距离
    int     type;           // 特征点类型
    int     shapeid;        // 捕捉到的图形
    int     handleIndex;    // 捕捉到控制点序号，最近点和垂足则为边号，交点则为另一图形的ID
    int     handleIndexSrc; // 待确定位置的源图形上的控制点序号，与handleIndex点匹配
    
    SnapItem() {}
    SnapItem(const Point2d& _pt, const Point2d& _base, float _dist, int _type = 0,
        int _shapeid = 0, int _handleIndex = -1, int _handleIndexSrc = -1)
        : pt(_pt), base(_base), maxdist(_dist), dist(_dist), type(_type), shapeid(_shapeid)
        , handleIndex(_handleIndex), handleIndexSrc(_handleIndexSrc) {}
};

static inline float diffx(const Point2d& pt1, const Point2d& pt2)
{
    return fabsf(pt1.x - pt2.x);
}

static inline float diffy(const Point2d& pt1, const Point2d& pt2)
{
    return fabsf(pt1.y - pt2.y);
}

static int snapHV(const Point2d& basePt, Point2d& newPt, SnapItem arr[3])
{
    int ret = 0;
    float d;
    
    d = arr[1].dist - diffx(newPt, basePt);
    if (d > _MGZERO || (d > - _MGZERO
                        && diffy(newPt, basePt) < diffy(newPt, arr[1].base))) {
        arr[1].dist = diffx(newPt, basePt);
        arr[1].base = basePt;
        newPt.x = basePt.x;
        arr[1].pt = newPt;
        arr[1].type = kMgSnapSameX;
        ret |= 1;
    }
    d = arr[2].dist - diffy(newPt, basePt);
    if (d > _MGZERO || (d > - _MGZERO
                        && diffx(newPt, basePt) < diffx(newPt, arr[2].base))) {
        arr[2].dist = diffy(newPt, basePt);
        arr[2].base = basePt;
        newPt.y = basePt.y;
        arr[2].pt = newPt;
        arr[2].type = kMgSnapSameY;
        ret |= 2;
    }
    
    return ret;
}

static bool skipShape(const int* ignoreids, const MgShape* sp)
{
    bool skip = sp->shapec()->getFlag(kMgNoSnap);
    for (int t = 0; ignoreids[t] != 0 && !skip; t++) {
        skip = (ignoreids[t] == sp->getID());           // 跳过当前图形
    }
    return skip;
}

static bool snapHandle(const MgMotion* sender, const Point2d& orgpt,
                       const MgShape* shape, int ignoreHd,
                       const MgShape* sp, SnapItem& arr0, Point2d* matchpt)
{
    bool ignored = sp->shapec()->isKindOf(MgSplines::Type()); // 除自由曲线外
    int n = ignored ? 0 : sp->shapec()->getHandleCount();
    bool dragHandle = (!shape || shape->getID() == 0 || // 正画的图形:末点动
                       orgpt == shape->shapec()->getHandlePoint(ignoreHd)); // 拖已有图形的点
    bool handleFound = false;
    
    for (int i = 0; i < n; i++) {                       // 循环每一个控制点
        Point2d pnt(sp->shapec()->getHandlePoint(i));   // 已有图形的一个控制点
        int handleType = sp->shapec()->getHandleType(i);
        
        float dist = pnt.distanceTo(orgpt);             // 触点与顶点匹配
        
        if (handleType == kMgHandleMidPoint) {          // 交点优先于中点
            dist += sender->displayMmToModel(0.5f);
        }
        if (dragHandle && arr0.dist > dist - _MGZERO
            && handleType < kMgHandleOutside
            && !(shape && shape->getID() == 0               // 新画线段的起点已
                 && shape->shapec()->getPointCount() > 1
                 && pnt == shape->shapec()->getPoint(0))) { // 与此点重合的除外
            arr0.dist = dist;
            arr0.base = orgpt;
            arr0.pt = pnt;
            arr0.type = kMgSnapPoint + handleType - kMgHandleVertext;
            arr0.shapeid = sp->getID();
            arr0.handleIndex = i;
            arr0.handleIndexSrc = dragHandle ? ignoreHd : -1;
            handleFound = true;
        }
        
        int d = matchpt ? shape->shapec()->getHandleCount() - 1 : -1;
        for (; d >= 0; d--) {                           // 整体移动图形，顶点匹配
            if (d == ignoreHd || shape->shapec()->isHandleFixed(d))
                continue;
            Point2d ptd (shape->shapec()->getHandlePoint(d));   // 当前图形的顶点
            
            dist = pnt.distanceTo(ptd);                 // 当前图形与其他图形顶点匹配
            if (handleType == kMgHandleMidPoint) {      // 交点优先于中点
                dist += sender->displayMmToModel(0.5f);
            }
            if (arr0.dist > dist - _MGZERO && handleType < kMgHandleOutside) {
                arr0.dist = dist;
                arr0.base = ptd;  // 新的移动起点为当前图形的一个顶点
                arr0.pt = pnt;    // 将从ptd移到其他图形顶点pnt
                arr0.type = kMgSnapPoint + handleType - kMgHandleVertext;
                arr0.shapeid = sp->getID();
                arr0.handleIndex = i;
                arr0.handleIndexSrc = d;
                handleFound = true;
                
                // 因为对当前图形先从startM移到pointM，然后再从pointM移到matchpt
                *matchpt = orgpt + (pnt - ptd);       // 所以最后差量为(pnt-ptd)
            }
        }
    }
    
    return handleFound;
}

static bool snapPerp(const MgMotion* sender, const Point2d& orgpt, const Tol& tol,
                     const MgShape* shape, const MgShape* sp, SnapItem& arr0,
                     bool perpOut, const Box2d& nearBox)
{
    int ret = -1;
    const MgBaseShape* s = sp->shapec();
    MgHitResult nearres;
    
    if (shape && shape->getID() == 0            // 正画的线段 与 折线类图形的边垂直
        && shape->shapec()->isKindOf(MgLine::Type()) && !s->isCurve()) {
        int n = s->getPointCount();
        Point2d perp1, perp2;
        const Point2d start(shape->shapec()->getPoint(0));
        
        for (int i = 0, edges = n - (s->isClosed() ? 0 : 1); i < edges; i++) {
            Point2d pt1(s->getHandlePoint(i));
            Point2d pt2(s->getHandlePoint((i + 1) % n));
            float d2 = mglnrel::ptToBeeline2(pt1, pt2, orgpt, perp2);
            
            if (mglnrel::isColinear2(pt1, pt2, start, tol)) {   // 起点在线上
                float dist = perp2.distanceTo(start) * 2;
                if (d2 > 2 * arr0.maxdist && arr0.dist > dist
                    && (perpOut || mglnrel::isProjectBetweenLine(pt1, pt2, perp2))) {
                    arr0.startpt = start;
                    arr0.dist = dist;                           // 当前点距捕捉点
                    arr0.base = start;                          // 起点同垂足
                    arr0.pt = start + orgpt - perp2.asVector(); // 投影到垂线
                    arr0.type = kMgSnapPerp;
                    arr0.shapeid = sp->getID();
                    arr0.handleIndex = i;
                    arr0.handleIndexSrc = -1;
                    ret = i;
                    
                    const MgShape* sp2 = sp->getParent()->hitTest(nearBox, nearres);
                    if (sp2 && sp2 != shape) {
                        pt1 = start;
                        pt2 = arr0.pt;
                        int n = MgEllipse::crossCircle(pt1, pt2, sp2->shapec());
                        if (n < 0) {
                            MgPath path1, path2;
                            
                            path1.moveTo(pt1);
                            path1.lineTo(pt1 + (pt2 - pt1) * 2.f);
                            
                            sp2->shapec()->output(path2);
                            path1.crossWithPath(path2, Box2d(orgpt, 1e10f, 0), arr0.pt);
                        } else if (n > 0) {
                            arr0.pt = pt2.distanceTo(orgpt) < pt1.distanceTo(orgpt) ? pt2 : pt1;
                        }
                        arr0.type = kMgSnapPerpNear;
                        arr0.handleIndexSrc = sp2->getID();
                    }
                }
            } else if (d2 < arr0.maxdist) {                     // 终点在线附近
                mglnrel::ptToBeeline2(pt1, pt2, start, perp1);
                float dist = perp1.distanceTo(orgpt);
                if (arr0.dist > dist
                    && (perpOut || mglnrel::isProjectBetweenLine(pt1, pt2, perp1))) {
                    arr0.startpt = start;
                    arr0.dist = dist;                           // 当前点距垂足
                    arr0.base = perp1;                          // 垂足为基点
                    arr0.pt   = arr0.base;                      // 也是捕捉点
                    arr0.type = kMgSnapPerp;
                    arr0.shapeid = sp->getID();
                    arr0.handleIndex = i;
                    arr0.handleIndexSrc = sp->getID();
                    ret = i;
                }
            }
        }
    }
    
    return ret >= 0;
}

static void snapNear(const MgMotion* sender, const Point2d& orgpt,
                     const MgShape* shape, int ignoreHd, float tolNear,
                     const MgShape* sp, SnapItem& arr0, Point2d* matchpt)
{
    if (arr0.type >= kMgSnapGrid && arr0.type < kMgSnapNearPt)
        return;
    
    Point2d ptd;
    MgHitResult res;
    const float mind = sender->displayMmToModel(4.f);
    float minDist = arr0.dist - (arr0.type == kMgSnapNearPt ? mind : 0.f);
    int d = matchpt ? shape->shapec()->getHandleCount() : 0;
    
    for (; d >= 0; d--) {       // 对需定位的图形(shape)的每个控制点和当前触点
        if (d == 0) {
            ptd = orgpt;        // 触点与边匹配
        }
        else {
            if (d - 1 == ignoreHd || shape->shapec()->isHandleFixed(d - 1))
                continue;
            ptd = shape->shapec()->getHandlePoint(d - 1);   // 控制点与边匹配
        }
        float dist = sp->shapec()->hitTest(ptd, tolNear, res);
        
        if (minDist > dist) {
            minDist = dist;
            arr0.dist = minDist + mind;
            arr0.base = ptd;            // 新的移动起点为当前图形的一个顶点
            arr0.pt = res.nearpt;       // 将从ptd移到其他图形顶点pnt
            arr0.type = kMgSnapNearPt;
            arr0.shapeid = sp->getID(); // 最近点在此图形上
            arr0.handleIndex = res.segment;
            arr0.handleIndexSrc = d - 1;
            if (matchpt) {  // 因为对当前图形先从startM移到pointM，然后再从pointM移到matchpt
                *matchpt = orgpt + (res.nearpt - ptd);
            }
        }
    }
}

static void snapGrid(const MgMotion*, const Point2d& orgpt,
                     const MgShape* shape, int ignoreHd,
                     const MgShape* sp, SnapItem arr[3], Point2d* matchpt)
{
    if (sp->shapec()->isKindOf(MgGrid::Type())) {
        Point2d newPt (orgpt);
        const MgGrid* grid = (const MgGrid*)(sp->shapec());
        
        Point2d dists(arr[1].dist, arr[2].dist);
        int type = grid->snap(newPt, dists);
        if (type & 1) {
            arr[1].base = newPt;
            arr[1].pt = newPt;
            arr[1].type = kMgSnapGridX;
            arr[1].dist = dists.x;
        }
        if (type & 2) {
            arr[2].base = newPt;
            arr[2].pt = newPt;
            arr[2].type = kMgSnapGridY;
            arr[2].dist = dists.y;
        }
        
        int d = matchpt ? shape->shapec()->getHandleCount() - 1 : -1;
        for (; d >= 0; d--) {
            if (d == ignoreHd || shape->shapec()->isHandleFixed(d))
                continue;
            
            Point2d ptd (shape->shapec()->getHandlePoint(d));
            dists.set(mgMin(arr[0].dist, arr[1].dist), mgMin(arr[0].dist, arr[2].dist));
            
            newPt = ptd;
            type = grid->snap(newPt, dists);
            float dist = newPt.distanceTo(ptd);
            
            if ((type & 3) == 3 && arr[0].dist > dist - _MGZERO) {
                arr[0].dist = dist;
                arr[0].base = ptd;
                arr[0].pt = newPt;
                arr[0].type = kMgSnapGrid;
                arr[0].shapeid = sp->getID();
                arr[0].handleIndex = -1;
                arr[0].handleIndexSrc = d;
                
                // 因为对当前图形先从startM移到pointM，然后再从pointM移到matchpt
                *matchpt = orgpt + (newPt - ptd);   // 所以最后差量为(pnt-ptd)
            }
        }
    }
}

static bool snapCross(const MgMotion* sender, const Point2d& orgpt,
                      const int* ignoreids, int ignoreHd,
                      const MgShape* shape, const MgShape* sp1,
                      SnapItem& arr0, Point2d* matchpt)
{
    MgShapeIterator it(sender->view->shapes());
    Point2d ptd, ptcross, pt1, pt2;
    int d = matchpt ? shape->shapec()->getHandleCount() : 0;
    int ret = 0;
    
    for (; d >= 0; d--) {       // 对需定位的图形(shape)的每个控制点和当前触点
        if (d == 0) {
            ptd = orgpt;        // 触点与交点匹配
        }
        else {
            if (d - 1 == ignoreHd || shape->shapec()->isHandleFixed(d - 1))
                continue;
            ptd = shape->shapec()->getHandlePoint(d - 1);   // 控制点与交点匹配
        }
        
        Box2d snapbox(orgpt, 2 * arr0.maxdist, 0);
        
        if (sp1->shapec()->getPointCount() < 2
            || !sp1->shapec()->hitTestBox(snapbox)) {
            continue;
        }
        
        MgPath path1;
        sp1->shapec()->output(path1);
        
        while (const MgShape* sp2 = it.getNext()) {
            if (skipShape(ignoreids, sp2) || sp2 == shape || sp2 == sp1
                || sp2->shapec()->getPointCount() < 2
                || !sp2->shapec()->hitTestBox(snapbox)) {
                continue;
            }
            
            int n = MgEllipse::crossCircle(pt1, pt2, sp1->shapec(), sp2->shapec(), orgpt);
            
            if (n < 0) {
                MgPath path2;
                sp2->shapec()->output(path2);
                n = path1.crossWithPath(path2, snapbox, ptcross) ? 1 : 0;
            } else if (n > 0) {
                ptcross = pt2.distanceTo(ptd) < pt1.distanceTo(ptd) ? pt2 : pt1;
                n = snapbox.contains(ptcross) ? 1 : 0;
            }
            
            if (n) {
                float dist = ptcross.distanceTo(ptd);
                if (arr0.dist > dist) {
                    arr0.dist = dist;
                    arr0.base = ptd;
                    arr0.pt = ptcross;
                    arr0.type = kMgSnapIntersect;
                    arr0.shapeid = sp1->getID();
                    arr0.handleIndex = sp2->getID();
                    arr0.handleIndexSrc = d - 1;
                    if (matchpt)
                        *matchpt = orgpt + (ptcross - ptd);
                    ret = sp2->getID();
                }
            }
        }
    }
    
    return ret != 0;
}

static void snapShape(const MgMotion* sender, const Point2d& orgpt,
                      float minBox, const Box2d& snapbox, const Box2d& wndbox,
                      bool needHandle, bool needNear, float tolNear,
                      bool needPerp, bool perpOut, const Tol& tolPerp,
                      bool needCross, const Box2d& nearBox, bool needGrid,
                      const MgShape* sp, const MgShape* shape, int ignoreHd,
                      const int* ignoreids, SnapItem arr[3], Point2d* matchpt)
{
    if (skipShape(ignoreids, sp) || sp == shape) {
        return;
    }
    
    Box2d extent(sp->shapec()->getExtent());
    int b = 0;
    
    if (sp->shapec()->getPointCount() > 1
        && extent.width() < minBox && extent.height() < minBox) { // 图形太小就跳过
        return;
    }
    if (extent.isIntersect(wndbox)) {
        b |= (needHandle && snapHandle(sender, orgpt, shape, ignoreHd,
                                       sp, arr[0], matchpt));
        b |= (needPerp && snapPerp(sender, orgpt, tolPerp, shape, sp,
                                   arr[0], perpOut, nearBox));
        b |= (needCross && snapCross(sender, orgpt, ignoreids, ignoreHd,
                                     shape, sp, arr[0], matchpt));
        if (!b && needNear) {
            snapNear(sender, orgpt, shape, ignoreHd, tolNear, sp, arr[0], matchpt);
        }
    }
    if (!b && needGrid && extent.isIntersect(snapbox)) {
        snapGrid(sender, orgpt, shape, ignoreHd, sp, arr, matchpt);
    }
}

static void snapPoints(const MgMotion* sender, const Point2d& orgpt,
                       const MgShape* shape, int ignoreHd,
                       const int* ignoreids, SnapItem arr[3], Point2d* matchpt)
{
    if (!sender->view->getOptionInt("snap", "snapEnabled", 1)) {
        return;
    }
    
    Box2d snapbox(orgpt, 2 * arr[0].dist, 0);       // 捕捉容差框
    GiTransform* xf = sender->view->xform();
    Box2d wndbox(xf->getWndRectM());
    MgShapeIterator it(sender->view->shapes());
    bool needHandle = !!sender->view->getOptionInt("snap", "snapHandle", 1);
    bool needNear = !!sender->view->getOptionInt("snap", "snapNear", 1);
    bool needPerp = !!sender->view->getOptionInt("snap", "snapPerp", 1);
    bool perpOut = !!sender->view->getOptionInt("snap", "perpOut", 0);
    bool needCross = !!sender->view->getOptionInt("snap", "snapCross", 1);
    float tolNear = sender->displayMmToModel("snap", "snapNearTol", 3.f);
    Tol tolPerp(sender->displayMmToModel(1));
    bool needGrid = !!sender->view->getOptionInt("snap", "snapGrid", 1);
    Box2d nearBox(orgpt, needNear ? mgMin(tolNear, sender->displayMmToModel(4.f)) : 0.f, 0);
    
    if (shape) {
        wndbox.unionWith(shape->shapec()->getExtent().inflate(arr[0].dist));
    }
    while (const MgShape* sp = it.getNext()) {
        snapShape(sender, orgpt, xf->displayToModel(2, true), snapbox, wndbox,
                  needHandle, needNear, tolNear, needPerp, perpOut, tolPerp,
                  needCross, nearBox, needGrid,
                  sp, shape, ignoreHd, ignoreids, arr, matchpt);
    }
}

// hotHandle: 绘新图时，起始步骤为-1，后续步骤>0；拖动一个或多个整体图形时为-1，拖动顶点时>=0
Point2d MgCmdManagerImpl::snapPoint(const MgMotion* sender, const Point2d& orgpt, const MgShape* shape,
                                    int hotHandle, int ignoreHd, const int* ignoreids)
{
    const int ignoreids_tmp[2] = { shape ? shape->getID() : 0, 0 };
    if (!ignoreids) ignoreids = ignoreids_tmp;
    
    if (!shape || hotHandle >= shape->shapec()->getHandleCount()) {
        hotHandle = -1;         // 对hotHandle进行越界检查
    }
    _ptSnap = orgpt;   // 默认结果为当前触点位置
    
    const float xytol = sender->displayMmToModel("snap", "snapPointTol", 4.f);
    const float xtol = sender->displayMmToModel("snap", "snapXTol", 1.f);
    SnapItem arr[3] = {         // 设置捕捉容差和捕捉初值
        SnapItem(_ptSnap, _ptSnap, xytol),                          // XY点捕捉
        SnapItem(_ptSnap, _ptSnap, xtol),                           // X分量捕捉，竖直线
        SnapItem(_ptSnap, _ptSnap, xtol),                           // Y分量捕捉，水平线
    };
    
    if (shape && shape->getID() == 0 && hotHandle > 0               // 绘图命令中的临时图形
        && !shape->shapec()->isCurve()                              // 是线段或折线
        && !shape->shapec()->isKindOf(MgBaseRect::Type())) {        // 不是矩形或椭圆
        Point2d pt (orgpt);
        snapHV(shape->shapec()->getPoint(hotHandle - 1), pt, arr);  // 和上一个点对齐
    }
    
    Point2d pnt(-1e10f, -1e10f);                    // 当前图形的某一个顶点匹配到其他顶点pnt
    bool matchpt = (shape && shape->getID() != 0    // 拖动整个图形
                    && (hotHandle < 0 || (ignoreHd >= 0 && ignoreHd != hotHandle)));
    
    snapPoints(sender, orgpt, shape, ignoreHd, ignoreids,
               arr, matchpt ? &pnt : NULL);         // 在所有图形中捕捉
    checkResult(arr);
    
    return matchpt && pnt.x > -1e8f ? pnt : _ptSnap;    // 顶点匹配优先于用触点捕捉结果
}

void MgCmdManagerImpl::checkResult(SnapItem arr[3])
{
    if (arr[0].type > 0) {                          // X和Y方向同时捕捉到一个点
        _ptSnap = arr[0].pt;                        // 结果点
        _snapBase[0] = arr[0].base;                 // 原始点
        _snapType[0] = arr[0].type;
        _snapShapeId = arr[0].shapeid;
        _snapHandle = arr[0].handleIndex;
        _snapHandleSrc = arr[0].handleIndexSrc;
        _startpt = arr[0].startpt;
    }
    else {
        _snapShapeId = 0;
        _snapHandle = -1;
        _snapHandleSrc = -1;
        
        _snapType[0] = arr[1].type;                 // 竖直方向捕捉到一个点
        if (arr[1].type > 0) {
            _ptSnap.x = arr[1].pt.x;
            _snapBase[0] = arr[1].base;
        }
        _snapType[1] = arr[2].type;                 // 水平方向捕捉到一个点
        if (arr[2].type > 0) {
            _ptSnap.y = arr[2].pt.y;
            _snapBase[1] = arr[2].base;
        }
    }
}

int MgCmdManagerImpl::getSnappedType() const
{
    if (_snapType[0] >= kMgSnapPoint)
        return _snapType[0];
    if (_snapType[0] == kMgSnapGridX && _snapType[1] == kMgSnapGridY)
        return kMgSnapGrid;
    return 0;
}

int MgCmdManagerImpl::getSnappedPoint(Point2d& fromPt, Point2d& toPt) const
{
    fromPt = _snapBase[0];
    toPt = _ptSnap;
    return getSnappedType();
}

bool MgCmdManagerImpl::getSnappedHandle(int& shapeid, int& handleIndex, int& handleIndexSrc) const
{
    shapeid = _snapShapeId;
    handleIndex = _snapHandle;
    handleIndexSrc = _snapHandleSrc;
    return shapeid != 0;
}

void MgCmdManagerImpl::clearSnap(const MgMotion* sender)
{
    if (_snapType[0] || _snapType[1]) {
        _snapType[0] = kMgSnapNone;
        _snapType[1] = kMgSnapNone;
        sender->view->redraw();
    }
}

static GiHandleTypes snapTypeToHandleType(int snapType)
{
    switch (snapType) {
        case kMgSnapPoint: return kGiHandleNode;
        case kMgSnapCenter: return kGiHandleCenter;
        case kMgSnapMidPoint: return kGiHandleMidPoint;
        case kMgSnapQuadrant: return kGiHandleQuadrant;
        case kMgSnapIntersect: return kGiHandleIntersect;
        case kMgSnapNearPt: return kGiHandleNear;
        default: return kGiHandleVertex;
    }
}

void MgCmdManagerImpl::drawPerpMark(const MgMotion* sender, GiGraphics* gs, GiContext& ctx) const
{
    const MgShape* sp = sender->view->shapes()->findShape(_snapShapeId);
    int n = sp ? sp->shapec()->getPointCount() : 0;
    float r = displayMmToModel(1.2f, gs);
    
    if (n > 1 && _snapHandle >= 0) {
        if (_snapType[0] == kMgSnapPerpNear) {
            gs->drawCircle(&ctx, _ptSnap, displayMmToModel(4.f, gs));
            gs->drawHandle(_ptSnap, kGiHandleNear);
        }
        
        Point2d pt1(sp->shapec()->getHandlePoint(_snapHandle));
        Point2d pt2(sp->shapec()->getHandlePoint((_snapHandle + 1) % n));
        
        Point2d dirpt(pt1.distanceTo(_snapBase[0])
                      > pt2.distanceTo(_snapBase[0]) ? pt1 : pt2);
        Point2d markpt1(_snapBase[0].rulerPoint(dirpt, 2 * r, 0));
        
        dirpt = _ptSnap == _snapBase[0] ? _startpt : _ptSnap;
        Point2d markpt3(_snapBase[0].rulerPoint(dirpt, 2 * r, 0));
        Point2d markpts[] = { markpt1, markpt1 + (markpt3 - _snapBase[0]), markpt3 };
        
        GiContext ctxmark(-2, GiColor(255, 255, 0, 200));
        gs->drawLines(&ctxmark, 3, markpts);
        
        ctx.setLineWidth(0, false);
        ctx.setLineStyle(GiContext::kSolidLine);
        gs->drawBeeline(&ctx, pt1, pt2);
        
        ctx.setFillAlpha(64);
        if (pt1.distanceTo(_snapBase[0]) > r)
            gs->drawCircle(&ctx, pt1, r);
        if (pt2.distanceTo(_snapBase[0]) > r)
            gs->drawCircle(&ctx, pt2, r);
    }
}

bool MgCmdManagerImpl::drawSnap(const MgMotion* sender, GiGraphics* gs) const
{
    bool ret = false;
    
    if (sender->dragging() || !sender->view->useFinger()) {
        if (_snapType[0] >= kMgSnapGrid) {
            bool small = (_snapType[0] >= kMgSnapNearPt || _snapType[0] < kMgSnapPoint);
            float r = displayMmToModel(small ? 3.f : 8.f, gs);
            GiContext ctx(-2, GiColor(0, 255, 0, 200), GiContext::kDashLine, GiColor(0, 200, 200, 32));
            
            if (_snapType[0] == kMgSnapPerp || _snapType[0] == kMgSnapPerpNear) {
                ret = gs->drawCircle(&ctx, _snapBase[0], r);
                drawPerpMark(sender, gs, ctx);
            } else {
                ret = gs->drawCircle(&ctx, _ptSnap, r);
                gs->drawHandle(_ptSnap, snapTypeToHandleType(_snapType[0]));
            }
        }
        else {
            GiContext ctx(0, GiColor(0, 255, 0, 200), GiContext::kDashLine, GiColor(0, 255, 0, 64));
            GiContext ctxcross(-2, GiColor(0, 255, 0, 200));
            
            if (_snapType[0] > 0) {
                if (_snapBase[0] == _ptSnap) {
                    if (_snapType[0] == kMgSnapGridX) {
                        Vector2d vec(0, displayMmToModel(15.f, gs));
                        ret = gs->drawLine(&ctxcross, _ptSnap - vec, _ptSnap + vec);
                        gs->drawCircle(&ctx, _snapBase[0], displayMmToModel(4.f, gs));
                    }
                }
                else {  // kMgSnapSameX
                    ret = gs->drawLine(&ctx, _snapBase[0], _ptSnap);
                    gs->drawCircle(&ctx, _snapBase[0], displayMmToModel(2.5f, gs));
                }
            }
            if (_snapType[1] > 0) {
                if (_snapBase[1] == _ptSnap) {
                    if (_snapType[1] == kMgSnapGridY) {
                        Vector2d vec(displayMmToModel(15.f, gs), 0);
                        ret = gs->drawLine(&ctxcross, _ptSnap - vec, _ptSnap + vec);
                        gs->drawCircle(&ctx, _snapBase[1], displayMmToModel(4.f, gs));
                    }
                }
                else {  // kMgSnapSameY
                    ret = gs->drawLine(&ctx, _snapBase[1], _ptSnap);
                    gs->drawCircle(&ctx, _snapBase[1], displayMmToModel(2.5f, gs));
                }
            }
        }
    }
    
    return ret;
}
