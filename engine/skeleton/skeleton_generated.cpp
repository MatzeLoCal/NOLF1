// GENERATED -- see gen_skeleton.py. Out-of-line definitions for
// non-pure virtuals, which the INTERFACE vtables require.

#include "skeleton_generated.h"

// ILTCSBase::WriteToMessageFloat -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::WriteToMessageByte -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::WriteToMessageWord -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::WriteToMessageDWord -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::WriteToMessageString -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::WriteToMessageVector -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::WriteToMessageCompVector -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::WriteToMessageCompPosition -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::WriteToMessageRotation -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::WriteToMessageHString -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::WriteToMessageHMessageWrite -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::WriteToMessageHMessageRead -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::WriteToMessageFormattedHString -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::WriteToMessageObject -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::WriteToLoadSaveMessageObject -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::ReadFromMessageFloat -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::ReadFromMessageByte -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::ReadFromMessageWord -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::ReadFromMessageDWord -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::ReadFromMessageVector -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::ReadFromMessageCompVector -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::ReadFromMessageCompPosition -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::ReadFromMessageRotation -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::ReadFromMessageObject -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::ReadFromMessageHString -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::ReadFromLoadSaveMessageObject -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::ReadFromMessageHMessageRead -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::EndHMessageRead -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::EndHMessageWrite -- hand-written in engine/shared/ltmessage.cpp
// ILTCSBase::ResetRead -- hand-written in engine/shared/ltmessage.cpp
LTRESULT ILTCSBase::GetRotationVectors(LTRotation *pRotation, LTVector *pUp, LTVector *pRight, LTVector *pForward) { LTSTUB("ILTCSBase::GetRotationVectors"); return LT_ERROR; }
void ILTCSBase::GetRotationVectorsFromMatrix(LTMatrix *pMat, LTVector *pUp, LTVector *pRight, LTVector *pForward) { LTSTUB("ILTCSBase::GetRotationVectorsFromMatrix"); }
void ILTCSBase::RotateAroundAxis(LTRotation *pRotation, LTVector *pAxis, float amount) { LTSTUB("ILTCSBase::RotateAroundAxis"); }
void ILTCSBase::EulerRotateX(LTRotation *pRotation, float amount) { LTSTUB("ILTCSBase::EulerRotateX"); }
void ILTCSBase::EulerRotateY(LTRotation *pRotation, float amount) { LTSTUB("ILTCSBase::EulerRotateY"); }
void ILTCSBase::EulerRotateZ(LTRotation *pRotation, float amount) { LTSTUB("ILTCSBase::EulerRotateZ"); }
void ILTCSBase::AlignRotation(LTRotation *pRotation, LTVector *pVector, LTVector *pUp) { LTSTUB("ILTCSBase::AlignRotation"); }
LTRESULT ILTCSBase::SetupEuler(LTRotation *pRotation, float pitch, float yaw, float roll) { LTSTUB("ILTCSBase::SetupEuler"); return LT_ERROR; }
LTRESULT ILTCSBase::InterpolateRotation(LTRotation *pDest, LTRotation *pRot1, LTRotation *pRot2, float t) { LTSTUB("ILTCSBase::InterpolateRotation"); return LT_ERROR; }
void ILTCSBase::SetupTransformationMatrix(LTMatrix *pMat, LTVector *pTranslation, LTRotation *pRotation) { LTSTUB("ILTCSBase::SetupTransformationMatrix"); }
void ILTCSBase::SetupTranslationMatrix(LTMatrix *pMat, LTVector *pTranslation) { LTSTUB("ILTCSBase::SetupTranslationMatrix"); }
void ILTCSBase::SetupRotationMatrix(LTMatrix *pMat, LTRotation *pRot) { LTSTUB("ILTCSBase::SetupRotationMatrix"); }
void ILTCSBase::SetupTranslationFromMatrix(LTVector *pTranslation, LTMatrix *pMat) { LTSTUB("ILTCSBase::SetupTranslationFromMatrix"); }
void ILTCSBase::SetupRotationFromMatrix(LTRotation *pRot, LTMatrix *pMat) { LTSTUB("ILTCSBase::SetupRotationFromMatrix"); }
void ILTCSBase::SetupRotationAroundPoint(LTMatrix *pMat, LTRotation *pRot, LTVector *pPoint) { LTSTUB("ILTCSBase::SetupRotationAroundPoint"); }
// ILTMath::GetRotationVectors -- hand-written in engine/kernel/math/ltmathlt.cpp
// ILTMath::InterpolateRotation -- hand-written in engine/kernel/math/ltmathlt.cpp
// ILTMath::SetupEuler -- hand-written in engine/kernel/math/ltmathlt.cpp
// ILTMath::SetupTransformationMatrix -- hand-written in engine/kernel/math/ltmathlt.cpp
// ILTMath::SetupTranslationMatrix -- hand-written in engine/kernel/math/ltmathlt.cpp
// ILTMath::SetupRotationMatrix -- hand-written in engine/kernel/math/ltmathlt.cpp
// ILTMath::SetupTranslationFromMatrix -- hand-written in engine/kernel/math/ltmathlt.cpp
// ILTMath::SetupRotationFromMatrix -- hand-written in engine/kernel/math/ltmathlt.cpp
// ILTMath::SetupRotationAroundPoint -- hand-written in engine/kernel/math/ltmathlt.cpp
// ILTMath::AlignRotation -- hand-written in engine/kernel/math/ltmathlt.cpp
// ILTMath::EulerRotateX -- hand-written in engine/kernel/math/ltmathlt.cpp
// ILTMath::EulerRotateY -- hand-written in engine/kernel/math/ltmathlt.cpp
// ILTMath::EulerRotateZ -- hand-written in engine/kernel/math/ltmathlt.cpp
// ILTMath::RotateAroundAxis -- hand-written in engine/kernel/math/ltmathlt.cpp
// ILTMath::GetRotationVectorsFromMatrix -- hand-written in engine/kernel/math/ltmathlt.cpp
LTRESULT ILTModel::GetSocket(HOBJECT hObj, char *pSocketName, HMODELSOCKET &hSocket) { LTSTUB("ILTModel::GetSocket"); return LT_ERROR; }
LTRESULT ILTModel::GetNode(HOBJECT hObj, char *pNodeName, HMODELNODE &hNode) { LTSTUB("ILTModel::GetNode"); return LT_ERROR; }
LTRESULT ILTModel::GetPiece(HOBJECT hObj, char *pPieceName, HMODELPIECE &hPiece) { LTSTUB("ILTModel::GetPiece"); return LT_ERROR; }
LTRESULT ILTModel::FindWeightSet(HOBJECT hObj, char *pSetName, HMODELWEIGHTSET &hSet) { LTSTUB("ILTModel::FindWeightSet"); return LT_ERROR; }
LTRESULT ILTModel::GetPieceHideStatus(HOBJECT hObj, HMODELPIECE hPiece, LTBOOL &bHidden) { LTSTUB("ILTModel::GetPieceHideStatus"); return LT_ERROR; }
LTRESULT ILTModel::SetPieceHideStatus(HOBJECT hObj, HMODELPIECE hPiece, LTBOOL bHidden) { LTSTUB("ILTModel::SetPieceHideStatus"); return LT_ERROR; }
LTRESULT ILTModel::GetNodeTransform(HOBJECT hObj, HMODELNODE hNode, LTransform &transform, LTBOOL bWorldSpace) { LTSTUB("ILTModel::GetNodeTransform"); return LT_ERROR; }
LTRESULT ILTModel::GetSocketTransform(HOBJECT hObj, HMODELSOCKET hSocket, LTransform &transform, LTBOOL bWorldSpace) { LTSTUB("ILTModel::GetSocketTransform"); return LT_ERROR; }
LTRESULT ILTModel::GetInstanceHookFn(HOBJECT hObj, ModelInstanceHookFn &fn) { LTSTUB("ILTModel::GetInstanceHookFn"); return LT_ERROR; }
LTRESULT ILTModel::SetInstanceHookFn(HOBJECT hObj, ModelInstanceHookFn fn) { LTSTUB("ILTModel::SetInstanceHookFn"); return LT_ERROR; }
LTRESULT ILTModel::GetMainTracker(HOBJECT hObj, LTAnimTracker* &pTracker) { LTSTUB("ILTModel::GetMainTracker"); return LT_ERROR; }
LTRESULT ILTModel::UpdateMainTracker(HOBJECT pObj, LTFLOAT fUpdateDelta) { LTSTUB("ILTModel::UpdateMainTracker"); return LT_ERROR; }
LTRESULT ILTModel::GetPlaybackState(LTAnimTracker *pTracker, uint32 &flags) { LTSTUB("ILTModel::GetPlaybackState"); return LT_ERROR; }
LTRESULT ILTModel::AddTracker(HOBJECT hObj, LTAnimTracker *pTracker) { LTSTUB("ILTModel::AddTracker"); return LT_ERROR; }
LTRESULT ILTModel::RemoveTracker(HOBJECT hObj, LTAnimTracker *pTracker) { LTSTUB("ILTModel::RemoveTracker"); return LT_ERROR; }
LTRESULT ILTModel::GetCurAnim(LTAnimTracker *pTracker, HMODELANIM &hAnim) { LTSTUB("ILTModel::GetCurAnim"); return LT_ERROR; }
LTRESULT ILTModel::SetCurAnim(LTAnimTracker *pTracker, HMODELANIM hAnim) { LTSTUB("ILTModel::SetCurAnim"); return LT_ERROR; }
LTRESULT ILTModel::ResetAnim(LTAnimTracker *pTracker) { LTSTUB("ILTModel::ResetAnim"); return LT_ERROR; }
LTRESULT ILTModel::GetLooping(LTAnimTracker *pTracker) { LTSTUB("ILTModel::GetLooping"); return LT_ERROR; }
LTRESULT ILTModel::SetLooping(LTAnimTracker *pTracker, LTBOOL bLooping) { LTSTUB("ILTModel::SetLooping"); return LT_ERROR; }
LTRESULT ILTModel::GetPlaying(LTAnimTracker *pTracker) { LTSTUB("ILTModel::GetPlaying"); return LT_ERROR; }
LTRESULT ILTModel::SetPlaying(LTAnimTracker *pTracker, LTBOOL bPlaying) { LTSTUB("ILTModel::SetPlaying"); return LT_ERROR; }
LTRESULT ILTModel::GetWeightSet(LTAnimTracker *pTracker, HMODELWEIGHTSET &hSet) { LTSTUB("ILTModel::GetWeightSet"); return LT_ERROR; }
LTRESULT ILTModel::SetWeightSet(LTAnimTracker *pTracker, HMODELWEIGHTSET hSet) { LTSTUB("ILTModel::SetWeightSet"); return LT_ERROR; }
LTRESULT ILTModel::GetCurAnimLength(LTAnimTracker *pTracker, uint32 &length) { LTSTUB("ILTModel::GetCurAnimLength"); return LT_ERROR; }
LTRESULT ILTModel::GetCurAnimTime(LTAnimTracker *pTracker, uint32 &curTime) { LTSTUB("ILTModel::GetCurAnimTime"); return LT_ERROR; }
LTRESULT ILTModel::SetCurAnimTime(LTAnimTracker *pTracker, uint32 curTime) { LTSTUB("ILTModel::SetCurAnimTime"); return LT_ERROR; }
LTRESULT ILTPhysics::GetFrictionCoefficient(HOBJECT hObj, float &coeff) { LTSTUB("ILTPhysics::GetFrictionCoefficient"); return LT_ERROR; }
LTRESULT ILTPhysics::SetFrictionCoefficient(HOBJECT hObj, float coeff) { LTSTUB("ILTPhysics::SetFrictionCoefficient"); return LT_ERROR; }
LTRESULT ILTPhysics::GetForceIgnoreLimit(HOBJECT hObj, float &limit) { LTSTUB("ILTPhysics::GetForceIgnoreLimit"); return LT_ERROR; }
LTRESULT ILTPhysics::SetForceIgnoreLimit(HOBJECT hObj, float limit) { LTSTUB("ILTPhysics::SetForceIgnoreLimit"); return LT_ERROR; }
LTRESULT ILTPhysics::GetVelocity(HOBJECT hObj, LTVector *pVel) { LTSTUB("ILTPhysics::GetVelocity"); return LT_ERROR; }
LTRESULT ILTPhysics::GetAcceleration(HOBJECT hObj, LTVector *pAccel) { LTSTUB("ILTPhysics::GetAcceleration"); return LT_ERROR; }
LTRESULT ILTPhysics::GetObjectMass(HOBJECT hObj, float &mass) { LTSTUB("ILTPhysics::GetObjectMass"); return LT_ERROR; }
LTRESULT ILTPhysics::SetObjectMass(HOBJECT hObj, float mass) { LTSTUB("ILTPhysics::SetObjectMass"); return LT_ERROR; }
LTRESULT ILTPhysics::GetObjectDims(HOBJECT hObj, LTVector *pNewDims) { LTSTUB("ILTPhysics::GetObjectDims"); return LT_ERROR; }
LTRESULT ILTPhysics::GetStandingOn(HOBJECT hObj, CollisionInfo *pInfo) { LTSTUB("ILTPhysics::GetStandingOn"); return LT_ERROR; }
LTRESULT ILTPhysics::IsWorldObject(HOBJECT hObj) { LTSTUB("ILTPhysics::IsWorldObject"); return LT_ERROR; }
LTRESULT ILTTransform::Get(LTransform &transform, LTVector &pos, LTRotation &rot) { LTSTUB("ILTTransform::Get"); return LT_ERROR; }
LTRESULT ILTTransform::Set(LTransform &transform, LTVector &pos, LTRotation &rot) { LTSTUB("ILTTransform::Set"); return LT_ERROR; }
LTRESULT ILTTransform::GetPos(LTransform &transform, LTVector &pos) { LTSTUB("ILTTransform::GetPos"); return LT_ERROR; }
LTRESULT ILTTransform::GetRot(LTransform &transform, LTRotation &rot) { LTSTUB("ILTTransform::GetRot"); return LT_ERROR; }
LTRESULT ILTTransform::ToMatrix(LTransform &transform, LTMatrix &mat) { LTSTUB("ILTTransform::ToMatrix"); return LT_ERROR; }
LTRESULT ILTTransform::FromMatrix(LTransform &transform, LTMatrix &mat) { LTSTUB("ILTTransform::FromMatrix"); return LT_ERROR; }
LTRESULT ILTTransform::Inverse(LTransform &inverse, LTransform &transform) { LTSTUB("ILTTransform::Inverse"); return LT_ERROR; }
LTRESULT ILTTransform::Multiply(LTransform &out, LTransform &t1, LTransform &t2) { LTSTUB("ILTTransform::Multiply"); return LT_ERROR; }
LTRESULT ILTTransform::Difference(LTransform &diff, LTransform &t1, LTransform &t2) { LTSTUB("ILTTransform::Difference"); return LT_ERROR; }
