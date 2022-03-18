#include "schemeshard__operation_part.h"
#include "schemeshard__operation_common.h"
#include "schemeshard_path_element.h"

#include "schemeshard_impl.h"

#include <ydb/core/base/path.h>
#include <ydb/core/protos/flat_tx_scheme.pb.h>
#include <ydb/core/protos/flat_scheme_op.pb.h>

namespace {


NKikimrSchemeOp::TModifyScheme MoveTableTask(NKikimr::NSchemeShard::TPath& src, NKikimr::NSchemeShard::TPath& dst) {
    NKikimrSchemeOp::TModifyScheme scheme;

    scheme.SetWorkingDir(dst.Parent().PathString());
    scheme.SetFailOnExist(true);
    scheme.SetOperationType(NKikimrSchemeOp::EOperationType::ESchemeOpMoveTable);
    auto operation = scheme.MutableMoveTable();
    operation->SetSrcPath(src.PathString());
    operation->SetDstPath(dst.PathString());

    return scheme;
}

NKikimrSchemeOp::TModifyScheme MoveTableIndexTask(NKikimr::NSchemeShard::TPath& src, NKikimr::NSchemeShard::TPath& dst) {
    NKikimrSchemeOp::TModifyScheme scheme;

    scheme.SetWorkingDir(dst.Parent().PathString());
    scheme.SetFailOnExist(true);
    scheme.SetOperationType(NKikimrSchemeOp::EOperationType::ESchemeOpMoveTableIndex);
    auto operation = scheme.MutableMoveTableIndex();
    operation->SetSrcPath(src.PathString());
    operation->SetDstPath(dst.PathString());

    return scheme;
}

}

namespace NKikimr {
namespace NSchemeShard {

TVector<ISubOperationBase::TPtr> CreateConsistentMoveTable(TOperationId nextId, const TTxTransaction& tx, TOperationContext& context) {
    Y_VERIFY(tx.GetOperationType() == NKikimrSchemeOp::EOperationType::ESchemeOpMoveTable);

    TVector<ISubOperationBase::TPtr> result;

    {
        TString errStr;
        if (!context.SS->CheckApplyIf(tx, errStr)) {
            return {CreateReject(nextId, NKikimrScheme::EStatus::StatusPreconditionFailed, errStr)};
        }
    }

    auto moving = tx.GetMoveTable();

    auto& srcStr = moving.GetSrcPath();
    auto& dstStr = moving.GetDstPath();

    TPath srcPath = TPath::Resolve(srcStr, context.SS);
    {
        TPath::TChecker checks = srcPath.Check();
        checks.IsResolved()
              .NotDeleted()
              .IsTable()
              .IsCommonSensePath();

        if (!checks) {
            TStringBuilder explain = TStringBuilder() << "src path fail checks"
                                           << ", path: " << srcStr;
            auto status = checks.GetStatus(&explain);
            return {CreateReject(nextId, status, explain)};
        }
    }

    {
        TStringBuilder explain = TStringBuilder() << "fail checks";

        if (!context.SS->CheckLocks(srcPath.Base()->PathId, tx, explain)) {
            return {CreateReject(nextId, NKikimrScheme::StatusMultipleModifications, explain)};
        }
    }

    TPath dstPath = TPath::Resolve(dstStr, context.SS);

    result.push_back(CreateMoveTable(TOperationId(nextId.GetTxId(),
                                               nextId.GetSubTxId() + result.size()),
                                     MoveTableTask(srcPath, dstPath)));

    for (auto& child: srcPath.Base()->GetChildren()) {
        auto name = child.first;

        TPath srcIndexPath = srcPath.Child(name);
        if (srcIndexPath.IsDeleted()) {
            continue;
        }

        TPath dstIndexPath = dstPath.Child(name);

        Y_VERIFY(srcIndexPath.Base()->PathId == child.second);
        Y_VERIFY_S(srcIndexPath.Base()->GetChildren().size() == 1,
                   srcIndexPath.PathString() << " has children " << srcIndexPath.Base()->GetChildren().size());

        result.push_back(CreateMoveTableIndex(TOperationId(nextId.GetTxId(),
                                                              nextId.GetSubTxId() + result.size()),
                                                 MoveTableIndexTask(srcIndexPath, dstIndexPath)));

        TString srcImplTableName = srcIndexPath.Base()->GetChildren().begin()->first;
        TPath srcImplTable = srcIndexPath.Child(srcImplTableName);
        if (srcImplTable.IsDeleted()) {
            continue;
        }
        Y_VERIFY(srcImplTable.Base()->PathId == srcIndexPath.Base()->GetChildren().begin()->second);

        TPath dstImplTable = dstIndexPath.Child(srcImplTableName);

        result.push_back(CreateMoveTable(TOperationId(nextId.GetTxId(),
                                                      nextId.GetSubTxId() + result.size()),
                                         MoveTableTask(srcImplTable, dstImplTable)));
    }

    return result;
}

}
}
