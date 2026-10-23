// ----------------------------------------------------------------------- //
//
// GENERATED FILE -- do not edit by hand.
//   Produced by engine/skeleton/gen_skeleton.py from LT2/sdk/inc/*.h
//   Re-run that script after any SDK header change.
//
// Concrete stand-ins for LT2's engine interfaces. Every method announces
// itself and returns LT_ERROR (see the script's header for why it is not
// LT_OK). The game will therefore stop at the first engine call it genuinely
// needs -- which names the next thing to implement.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_ENGINE_SKELETON_GENERATED_H__
#define __LT_ENGINE_SKELETON_GENERATED_H__

// ⚠️ Include order mirrors the game's own Stdafx.h: windows.h first so
// _WINDOWS_ is defined, then the SDK. Without it ltbasedefs.h cannot see
// _MAX_PATH and mfcstub declares its own POINT/RECT.
#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

// The interfaces these stubs derive from.
#include "iltcsbase.h"
#include "iltclient.h"
#include "iltserver.h"
#include "iltcommon.h"
#include "iltmodel.h"
#include "iltphysics.h"
#include "iltsoundmgr.h"
#include "ilttransform.h"
#include "iltmath.h"
#include "iltmessage.h"
#include "iltcursor.h"
#include "iltcustomdraw.h"
#include "iltdirectmusic.h"
#include "iltlightanim.h"
#include "iltprelight.h"
#include "iltspritecontrol.h"
#include "ilttexmod.h"
#include "iltvideomgr.h"

#include "ltstub.h"

// LTStub_ILTCSBase -- from iltcsbase.h
class LTStub_ILTCSBase : public ILTCSBase
{
public:
    HMESSAGEWRITE StartHMessageWrite() override { LTSTUB("ILTCSBase::StartHMessageWrite"); return (HMESSAGEWRITE)0; }
    HMODELANIM GetAnimIndex(HOBJECT hObj, char *pAnimName) override { LTSTUB("ILTCSBase::GetAnimIndex"); return (HMODELANIM)0; }
    void SetModelAnimation(HOBJECT hObj, HMODELANIM hAnim) override { LTSTUB("ILTCSBase::SetModelAnimation"); }
    HMODELANIM GetModelAnimation(HOBJECT hObj) override { LTSTUB("ILTCSBase::GetModelAnimation"); return (HMODELANIM)0; }
    void SetModelLooping(HOBJECT hObj, LTBOOL bLoop) override { LTSTUB("ILTCSBase::SetModelLooping"); }
    LTBOOL GetModelLooping(HOBJECT hObj) override { LTSTUB("ILTCSBase::GetModelLooping"); return LTFALSE; }
    LTRESULT ResetModelAnimation(HOBJECT hObj) override { LTSTUB("ILTCSBase::ResetModelAnimation"); return LT_ERROR; }
    uint32 GetModelPlaybackState(HOBJECT hObj) override { LTSTUB("ILTCSBase::GetModelPlaybackState"); return (uint32)0; }
    LTRESULT FreeUnusedModels() override { LTSTUB("ILTCSBase::FreeUnusedModels"); return LT_ERROR; }
    void CPrint(char *pMsg, ...) override { LTSTUB("ILTCSBase::CPrint"); }
    uint32 GetPointContainers(LTVector *pPoint, HOBJECT *pList, uint32 maxListSize) override { LTSTUB("ILTCSBase::GetPointContainers"); return (uint32)0; }
    LTBOOL GetContainerCode(HOBJECT hObj, uint16 *pCode) override { LTSTUB("ILTCSBase::GetContainerCode"); return LTFALSE; }
    LTRESULT OpenFile(char *pFilename, ILTStream **pStream) override { LTSTUB("ILTCSBase::OpenFile"); return LT_ERROR; }
    LTRESULT CopyFile(const char *pszSourceFile, const char *pszDestFile) override { LTSTUB("ILTCSBase::CopyFile"); return LT_ERROR; }
    HSTRING FormatString(int messageCode, ...) override { LTSTUB("ILTCSBase::FormatString"); return (HSTRING)0; }
    HSTRING CopyString(HSTRING hString) override { LTSTUB("ILTCSBase::CopyString"); return (HSTRING)0; }
    HSTRING CreateString(char *pString) override { LTSTUB("ILTCSBase::CreateString"); return (HSTRING)0; }
    void FreeString(HSTRING hString) override { LTSTUB("ILTCSBase::FreeString"); }
    LTBOOL CompareStrings(HSTRING hString1, HSTRING hString2) override { LTSTUB("ILTCSBase::CompareStrings"); return LTFALSE; }
    LTBOOL CompareStringsUpper(HSTRING hString1, HSTRING hString2) override { LTSTUB("ILTCSBase::CompareStringsUpper"); return LTFALSE; }
    char* GetStringData(HSTRING hString) override { LTSTUB("ILTCSBase::GetStringData"); return 0; }
    float GetVarValueFloat(HCONSOLEVAR hVar) override { LTSTUB("ILTCSBase::GetVarValueFloat"); return 0.0f; }
    char* GetVarValueString(HCONSOLEVAR hVar) override { LTSTUB("ILTCSBase::GetVarValueString"); return 0; }
    LTFLOAT GetTime() override { LTSTUB("ILTCSBase::GetTime"); return 0.0f; }
    LTFLOAT GetFrameTime() override { LTSTUB("ILTCSBase::GetFrameTime"); return 0.0f; }
    LTRESULT RemoveObject(HOBJECT hObj) override { LTSTUB("ILTCSBase::RemoveObject"); return LT_ERROR; }
    LTRESULT GetRotationVectors(LTRotation *pRotation, LTVector *pUp, LTVector *pRight, LTVector *pForward) override { LTSTUB("ILTCSBase::GetRotationVectors"); return LT_ERROR; }
    void GetRotationVectorsFromMatrix(LTMatrix *pMat, LTVector *pUp, LTVector *pRight, LTVector *pForward) override { LTSTUB("ILTCSBase::GetRotationVectorsFromMatrix"); }
    void RotateAroundAxis(LTRotation *pRotation, LTVector *pAxis, float amount) override { LTSTUB("ILTCSBase::RotateAroundAxis"); }
    void EulerRotateX(LTRotation *pRotation, float amount) override { LTSTUB("ILTCSBase::EulerRotateX"); }
    void EulerRotateY(LTRotation *pRotation, float amount) override { LTSTUB("ILTCSBase::EulerRotateY"); }
    void EulerRotateZ(LTRotation *pRotation, float amount) override { LTSTUB("ILTCSBase::EulerRotateZ"); }
    void AlignRotation(LTRotation *pRotation, LTVector *pVector, LTVector *pUp) override { LTSTUB("ILTCSBase::AlignRotation"); }
    LTRESULT SetupEuler(LTRotation *pRotation, float pitch, float yaw, float roll) override { LTSTUB("ILTCSBase::SetupEuler"); return LT_ERROR; }
    LTRESULT InterpolateRotation(LTRotation *pDest, LTRotation *pRot1, LTRotation *pRot2, float t) override { LTSTUB("ILTCSBase::InterpolateRotation"); return LT_ERROR; }
    void SetupTransformationMatrix(LTMatrix *pMat, LTVector *pTranslation, LTRotation *pRotation) override { LTSTUB("ILTCSBase::SetupTransformationMatrix"); }
    void SetupTranslationMatrix(LTMatrix *pMat, LTVector *pTranslation) override { LTSTUB("ILTCSBase::SetupTranslationMatrix"); }
    void SetupRotationMatrix(LTMatrix *pMat, LTRotation *pRot) override { LTSTUB("ILTCSBase::SetupRotationMatrix"); }
    void SetupTranslationFromMatrix(LTVector *pTranslation, LTMatrix *pMat) override { LTSTUB("ILTCSBase::SetupTranslationFromMatrix"); }
    void SetupRotationFromMatrix(LTRotation *pRot, LTMatrix *pMat) override { LTSTUB("ILTCSBase::SetupRotationFromMatrix"); }
    void SetupRotationAroundPoint(LTMatrix *pMat, LTRotation *pRot, LTVector *pPoint) override { LTSTUB("ILTCSBase::SetupRotationAroundPoint"); }

    // Sub-interface DATA members. ⚠️ Not methods and not
    // function pointers -- reached through INLINE accessors, so
    // nothing here ever passes through a stub. Left NULL they are
    // a null read with no [LTSTUB] line before it.
    //
    // Call this from the most-derived constructor, BEFORE any
    // deliberate per-member override: the last assignment wins, and
    // the host has reasons to prefer some variants (the CLIENT
    // flavours of ILTPhysics and ILTSoundMgr, for instance).
    //
    // ⚠️ DECLARED here, DEFINED at the bottom of this header. The
    // stub types it assigns are declared LATER in the file, and an
    // inline body would not see them: a member function body is
    // parsed in its own class's complete-class context, which
    // delays lookup for THAT class's members only, not for types
    // that appear further down.
    void LTSkel_WireData();
};

// LTStub_ILTClient -- from iltclient.h
class LTStub_ILTClient : public ILTClient
{
public:
    LTRESULT GetPointStatus(LTVector *pPoint) override { LTSTUB("ILTClient::GetPointStatus"); return LT_ERROR; }
    LTRESULT GetPointShade(LTVector *pPoint, LTVector *pColor) override { LTSTUB("ILTClient::GetPointShade"); return LT_ERROR; }
    LTRESULT GetSConValueFloat(char *pName, float &val) override { LTSTUB("ILTClient::GetSConValueFloat"); return LT_ERROR; }
    LTRESULT GetSConValueString(char *pName, char *valBuf, uint32 bufLen) override { LTSTUB("ILTClient::GetSConValueString"); return LT_ERROR; }
    float GetServerConVarValueFloat(char *pName) override { LTSTUB("ILTClient::GetServerConVarValueFloat"); return 0.0f; }
    char* GetServerConVarValueString(char *pName) override { LTSTUB("ILTClient::GetServerConVarValueString"); return 0; }
    LTRESULT GetGlobalLightDir(LTVector &dir) override { LTSTUB("ILTClient::GetGlobalLightDir"); return LT_ERROR; }
    LTRESULT SetGlobalLightDir(LTVector dir) override { LTSTUB("ILTClient::SetGlobalLightDir"); return LT_ERROR; }
    LTRESULT GetGlobalLightColor(LTVector &color) override { LTSTUB("ILTClient::GetGlobalLightColor"); return LT_ERROR; }
    LTRESULT SetGlobalLightColor(LTVector color) override { LTSTUB("ILTClient::SetGlobalLightColor"); return LT_ERROR; }
    LTRESULT GetAmbientLight(float &light) override { LTSTUB("ILTClient::GetAmbientLight"); return LT_ERROR; }
    LTRESULT SetAmbientLight(float light) override { LTSTUB("ILTClient::SetAmbientLight"); return LT_ERROR; }
    LTRESULT StartVideo(char *pFilename, uint32 flags) override { LTSTUB("ILTClient::StartVideo"); return LT_ERROR; }
    LTRESULT StopVideo() override { LTSTUB("ILTClient::StopVideo"); return LT_ERROR; }
    LTRESULT UpdateVideo() override { LTSTUB("ILTClient::UpdateVideo"); return LT_ERROR; }
    LTRESULT IsVideoPlaying() override { LTSTUB("ILTClient::IsVideoPlaying"); return LT_ERROR; }
    HMESSAGEWRITE StartMessage(uint8 messageID) override { LTSTUB("ILTClient::StartMessage"); return (HMESSAGEWRITE)0; }
    LTRESULT EndMessage(HMESSAGEWRITE hMessage) override { LTSTUB("ILTClient::EndMessage"); return LT_ERROR; }
    LTRESULT EndMessage2(HMESSAGEWRITE hMessage, uint32 flags) override { LTSTUB("ILTClient::EndMessage2"); return LT_ERROR; }
    LTRESULT SendToServer(ILTMessage &msg, uint8 msgID, uint32 flags) override { LTSTUB("ILTClient::SendToServer"); return LT_ERROR; }
    LTRESULT GetAttachments(HLOCALOBJ hObj, HLOCALOBJ *inList, uint32 inListSize, uint32 *outListSize, uint32 *outNumAttachments) override { LTSTUB("ILTClient::GetAttachments"); return LT_ERROR; }
    LTRESULT ProcessAttachments(HOBJECT hObj) override { LTSTUB("ILTClient::ProcessAttachments"); return LT_ERROR; }
    LTRESULT SetObjectPos(HLOCALOBJ hObj, LTVector *pPos, LTBOOL bForce=LTFALSE) override { LTSTUB("ILTClient::SetObjectPos"); return LT_ERROR; }
    uint32 GetObjectFlags(HOBJECT hObj) override { LTSTUB("ILTClient::GetObjectFlags"); return (uint32)0; }
    void SetObjectFlags(HOBJECT hObj, uint32 flags) override { LTSTUB("ILTClient::SetObjectFlags"); }
    LTParticle* AddParticle(HLOCALOBJ hObj, LTVector *pPos, LTVector *pVelocity, LTVector *pColor, float lifeTime) override { LTSTUB("ILTClient::AddParticle"); return 0; }
    LTRESULT GetSpriteControl(HLOCALOBJ hObj, ILTSpriteControl* &pControl) override { LTSTUB("ILTClient::GetSpriteControl"); return LT_ERROR; }
    LTRESULT GetCanvasFn(HOBJECT hCanvas, CanvasDrawFn &fn, void* &pUserData) override { LTSTUB("ILTClient::GetCanvasFn"); return LT_ERROR; }
    LTRESULT SetCanvasFn(HOBJECT hCanvas, CanvasDrawFn fn, void* pUserData) override { LTSTUB("ILTClient::SetCanvasFn"); return LT_ERROR; }
    LTRESULT GetCanvasRadius(HOBJECT hCanvas, float &radius) override { LTSTUB("ILTClient::GetCanvasRadius"); return LT_ERROR; }
    LTRESULT SetCanvasRadius(HOBJECT hCanvas, float radius) override { LTSTUB("ILTClient::SetCanvasRadius"); return LT_ERROR; }
    LTRESULT ModelNodeControl(HOBJECT hObj, NodeControlFn fn, void *pUserData) override { LTSTUB("ILTClient::ModelNodeControl"); return LT_ERROR; }
    uint32 GetModelAnimation(HLOCALOBJ hObj) override { LTSTUB("ILTClient::GetModelAnimation"); return (uint32)0; }
    void SetModelAnimation(HLOCALOBJ hObj, uint32 iAnim) override { LTSTUB("ILTClient::SetModelAnimation"); }
    LTRESULT ResetModelAnimation(HLOCALOBJ hObj) override { LTSTUB("ILTClient::ResetModelAnimation"); return LT_ERROR; }
    uint32 GetModelPlaybackState(HLOCALOBJ hObj) override { LTSTUB("ILTClient::GetModelPlaybackState"); return (uint32)0; }
    LTBOOL GetModelLooping(HLOCALOBJ hObj) override { LTSTUB("ILTClient::GetModelLooping"); return LTFALSE; }
    void SetModelLooping(HLOCALOBJ hObj, LTBOOL bLoop) override { LTSTUB("ILTClient::SetModelLooping"); }
    LTBOOL GetModelPlaying(HLOCALOBJ hObj) override { LTSTUB("ILTClient::GetModelPlaying"); return LTFALSE; }
    void SetModelPlaying(HLOCALOBJ hObj, LTBOOL bPlaying) override { LTSTUB("ILTClient::SetModelPlaying"); }
    LTRESULT StartQuery(char *pInfo) override { LTSTUB("ILTClient::StartQuery"); return LT_ERROR; }
    LTRESULT UpdateQuery() override { LTSTUB("ILTClient::UpdateQuery"); return LT_ERROR; }
    LTRESULT GetQueryResults(NetSession* &pListHead) override { LTSTUB("ILTClient::GetQueryResults"); return LT_ERROR; }
    LTRESULT EndQuery() override { LTSTUB("ILTClient::EndQuery"); return LT_ERROR; }
    HMESSAGEWRITE StartHMessageWrite() override { LTSTUB("ILTCSBase::StartHMessageWrite"); return (HMESSAGEWRITE)0; }
    HMODELANIM GetAnimIndex(HOBJECT hObj, char *pAnimName) override { LTSTUB("ILTCSBase::GetAnimIndex"); return (HMODELANIM)0; }
    LTRESULT FreeUnusedModels() override { LTSTUB("ILTCSBase::FreeUnusedModels"); return LT_ERROR; }
    void CPrint(char *pMsg, ...) override { LTSTUB("ILTCSBase::CPrint"); }
    uint32 GetPointContainers(LTVector *pPoint, HOBJECT *pList, uint32 maxListSize) override { LTSTUB("ILTCSBase::GetPointContainers"); return (uint32)0; }
    LTBOOL GetContainerCode(HOBJECT hObj, uint16 *pCode) override { LTSTUB("ILTCSBase::GetContainerCode"); return LTFALSE; }
    LTRESULT OpenFile(char *pFilename, ILTStream **pStream) override { LTSTUB("ILTCSBase::OpenFile"); return LT_ERROR; }
    LTRESULT CopyFile(const char *pszSourceFile, const char *pszDestFile) override { LTSTUB("ILTCSBase::CopyFile"); return LT_ERROR; }
    HSTRING FormatString(int messageCode, ...) override { LTSTUB("ILTCSBase::FormatString"); return (HSTRING)0; }
    HSTRING CopyString(HSTRING hString) override { LTSTUB("ILTCSBase::CopyString"); return (HSTRING)0; }
    HSTRING CreateString(char *pString) override { LTSTUB("ILTCSBase::CreateString"); return (HSTRING)0; }
    void FreeString(HSTRING hString) override { LTSTUB("ILTCSBase::FreeString"); }
    LTBOOL CompareStrings(HSTRING hString1, HSTRING hString2) override { LTSTUB("ILTCSBase::CompareStrings"); return LTFALSE; }
    LTBOOL CompareStringsUpper(HSTRING hString1, HSTRING hString2) override { LTSTUB("ILTCSBase::CompareStringsUpper"); return LTFALSE; }
    char* GetStringData(HSTRING hString) override { LTSTUB("ILTCSBase::GetStringData"); return 0; }
    float GetVarValueFloat(HCONSOLEVAR hVar) override { LTSTUB("ILTCSBase::GetVarValueFloat"); return 0.0f; }
    char* GetVarValueString(HCONSOLEVAR hVar) override { LTSTUB("ILTCSBase::GetVarValueString"); return 0; }
    LTFLOAT GetTime() override { LTSTUB("ILTCSBase::GetTime"); return 0.0f; }
    LTFLOAT GetFrameTime() override { LTSTUB("ILTCSBase::GetFrameTime"); return 0.0f; }
    LTRESULT RemoveObject(HOBJECT hObj) override { LTSTUB("ILTCSBase::RemoveObject"); return LT_ERROR; }
    LTRESULT GetRotationVectors(LTRotation *pRotation, LTVector *pUp, LTVector *pRight, LTVector *pForward) override { LTSTUB("ILTCSBase::GetRotationVectors"); return LT_ERROR; }
    void GetRotationVectorsFromMatrix(LTMatrix *pMat, LTVector *pUp, LTVector *pRight, LTVector *pForward) override { LTSTUB("ILTCSBase::GetRotationVectorsFromMatrix"); }
    void RotateAroundAxis(LTRotation *pRotation, LTVector *pAxis, float amount) override { LTSTUB("ILTCSBase::RotateAroundAxis"); }
    void EulerRotateX(LTRotation *pRotation, float amount) override { LTSTUB("ILTCSBase::EulerRotateX"); }
    void EulerRotateY(LTRotation *pRotation, float amount) override { LTSTUB("ILTCSBase::EulerRotateY"); }
    void EulerRotateZ(LTRotation *pRotation, float amount) override { LTSTUB("ILTCSBase::EulerRotateZ"); }
    void AlignRotation(LTRotation *pRotation, LTVector *pVector, LTVector *pUp) override { LTSTUB("ILTCSBase::AlignRotation"); }
    LTRESULT SetupEuler(LTRotation *pRotation, float pitch, float yaw, float roll) override { LTSTUB("ILTCSBase::SetupEuler"); return LT_ERROR; }
    LTRESULT InterpolateRotation(LTRotation *pDest, LTRotation *pRot1, LTRotation *pRot2, float t) override { LTSTUB("ILTCSBase::InterpolateRotation"); return LT_ERROR; }
    void SetupTransformationMatrix(LTMatrix *pMat, LTVector *pTranslation, LTRotation *pRotation) override { LTSTUB("ILTCSBase::SetupTransformationMatrix"); }
    void SetupTranslationMatrix(LTMatrix *pMat, LTVector *pTranslation) override { LTSTUB("ILTCSBase::SetupTranslationMatrix"); }
    void SetupRotationMatrix(LTMatrix *pMat, LTRotation *pRot) override { LTSTUB("ILTCSBase::SetupRotationMatrix"); }
    void SetupTranslationFromMatrix(LTVector *pTranslation, LTMatrix *pMat) override { LTSTUB("ILTCSBase::SetupTranslationFromMatrix"); }
    void SetupRotationFromMatrix(LTRotation *pRot, LTMatrix *pMat) override { LTSTUB("ILTCSBase::SetupRotationFromMatrix"); }
    void SetupRotationAroundPoint(LTMatrix *pMat, LTRotation *pRot, LTVector *pPoint) override { LTSTUB("ILTCSBase::SetupRotationAroundPoint"); }

    // Sub-interface DATA members. ⚠️ Not methods and not
    // function pointers -- reached through INLINE accessors, so
    // nothing here ever passes through a stub. Left NULL they are
    // a null read with no [LTSTUB] line before it.
    //
    // Call this from the most-derived constructor, BEFORE any
    // deliberate per-member override: the last assignment wins, and
    // the host has reasons to prefer some variants (the CLIENT
    // flavours of ILTPhysics and ILTSoundMgr, for instance).
    //
    // ⚠️ DECLARED here, DEFINED at the bottom of this header. The
    // stub types it assigns are declared LATER in the file, and an
    // inline body would not see them: a member function body is
    // parsed in its own class's complete-class context, which
    // delays lookup for THAT class's members only, not for types
    // that appear further down.
    void LTSkel_WireData();
};

// LTStub_ILTServer -- from iltserver.h
class LTStub_ILTServer : public ILTServer
{
public:
    LTRESULT GetNumClassProps(const HCLASS hClass, uint32 &count) override { LTSTUB("ILTServer::GetNumClassProps"); return LT_ERROR; }
    LTRESULT GetClassProp(const HCLASS hClass, const uint32 iProp, ClassPropInfo &info) override { LTSTUB("ILTServer::GetClassProp"); return LT_ERROR; }
    LTRESULT GetClassName(const HCLASS hClass, char *pName, uint32 maxNameBytes) override { LTSTUB("ILTServer::GetClassName"); return LT_ERROR; }
    LTRESULT GetGlobalForce(LTVector *pVec) override { LTSTUB("ILTServer::GetGlobalForce"); return LT_ERROR; }
    LTRESULT SetGlobalForce(LTVector *pVec) override { LTSTUB("ILTServer::SetGlobalForce"); return LT_ERROR; }
    LTRESULT ThreadLoadFile(char *pFilename, uint32 type) override { LTSTUB("ILTServer::ThreadLoadFile"); return LT_ERROR; }
    LTRESULT UnloadFile(char *pFilename, uint32 type) override { LTSTUB("ILTServer::UnloadFile"); return LT_ERROR; }
    LTRESULT GetHPolyObject(const HPOLY hPoly, HOBJECT &hObject) override { LTSTUB("ILTServer::GetHPolyObject"); return LT_ERROR; }
    LTRESULT FindNamedObjects(char *pName, BaseObjArray<HOBJECT> &objArray, uint32 *nTotalFound=NULL) override { LTSTUB("ILTServer::FindNamedObjects"); return LT_ERROR; }
    LTRESULT FindWorldModelObjectIntersections(HOBJECT hWorldModel, LTVector vNewPos, LTRotation rNewRot, BaseObjArray<HOBJECT> &objArray) override { LTSTUB("ILTServer::FindWorldModelObjectIntersections"); return LT_ERROR; }
    LTRESULT GetWorldBox(LTVector &min, LTVector &max) override { LTSTUB("ILTServer::GetWorldBox"); return LT_ERROR; }
    HMESSAGEWRITE StartSpecialEffectMessage(LPBASECLASS pObject) override { LTSTUB("ILTServer::StartSpecialEffectMessage"); return (HMESSAGEWRITE)0; }
    HMESSAGEWRITE StartInstantSpecialEffectMessage(LTVector *pPos) override { LTSTUB("ILTServer::StartInstantSpecialEffectMessage"); return (HMESSAGEWRITE)0; }
    HMESSAGEWRITE StartMessageToObject(LPBASECLASS pSender, HOBJECT hSendTo, uint32 messageID) override { LTSTUB("ILTServer::StartMessageToObject"); return (HMESSAGEWRITE)0; }
    LTRESULT StartMessageToServer(LPBASECLASS pSender, uint32 messageID, HMESSAGEWRITE *hWrite) override { LTSTUB("ILTServer::StartMessageToServer"); return LT_ERROR; }
    HMESSAGEWRITE StartMessage(HCLIENT hSendTo, uint8 messageID) override { LTSTUB("ILTServer::StartMessage"); return (HMESSAGEWRITE)0; }
    LTRESULT EndMessage2(HMESSAGEWRITE hMessage, uint32 flags) override { LTSTUB("ILTServer::EndMessage2"); return LT_ERROR; }
    LTRESULT EndMessage(HMESSAGEWRITE hMessage) override { LTSTUB("ILTServer::EndMessage"); return LT_ERROR; }
    LTRESULT SetObjectSFXMessage(HOBJECT hObject, ILTMessage &msg) override { LTSTUB("ILTServer::SetObjectSFXMessage"); return LT_ERROR; }
    LTRESULT SendToObject(ILTMessage &msg, uint32 msgID, HOBJECT hSender, HOBJECT hSendTo, uint32 flags) override { LTSTUB("ILTServer::SendToObject"); return LT_ERROR; }
    LTRESULT SendToServer(ILTMessage &msg, uint32 msgID, HOBJECT hSender, uint32 flags) override { LTSTUB("ILTServer::SendToServer"); return LT_ERROR; }
    LTRESULT SendToClient(ILTMessage &msg, uint8 msgID, HCLIENT hSendTo, uint32 flags) override { LTSTUB("ILTServer::SendToClient"); return LT_ERROR; }
    LTRESULT SendSFXMessage(ILTMessage &msg, LTVector &pos, uint32 flags) override { LTSTUB("ILTServer::SendSFXMessage"); return LT_ERROR; }
    LTRESULT SendTo(const void *pData, uint32 len, const char *sAddr, uint32 port) override { LTSTUB("ILTServer::SendTo"); return LT_ERROR; }
    LTRESULT GetClientPing(HCLIENT hClient, float &ping) override { LTSTUB("ILTServer::GetClientPing"); return LT_ERROR; }
    float GetObjectMass(HOBJECT hObj) override { LTSTUB("ILTServer::GetObjectMass"); return 0.0f; }
    void SetObjectMass(HOBJECT hObj, float mass) override { LTSTUB("ILTServer::SetObjectMass"); }
    float GetForceIgnoreLimit(HOBJECT hObj, float &limit) override { LTSTUB("ILTServer::GetForceIgnoreLimit"); return 0.0f; }
    void SetForceIgnoreLimit(HOBJECT hObj, float limit) override { LTSTUB("ILTServer::SetForceIgnoreLimit"); }
    char* GetObjectName(HOBJECT hObject) override { LTSTUB("ILTServer::GetObjectName"); return 0; }
    LTRESULT GetObjectName(HOBJECT hObject, char *pName, uint32 nameBufSize) override { LTSTUB("ILTServer::GetObjectName"); return LT_ERROR; }
    LTRESULT SetFrictionCoefficient(HOBJECT hObj, float coeff) override { LTSTUB("ILTServer::SetFrictionCoefficient"); return LT_ERROR; }
    LTRESULT MoveObject(HOBJECT hObj, LTVector *pNewPos) override { LTSTUB("ILTServer::MoveObject"); return LT_ERROR; }
    LTRESULT GetStandingOn(HOBJECT hObj, CollisionInfo *pInfo) override { LTSTUB("ILTServer::GetStandingOn"); return LT_ERROR; }
    uint32 GetObjectFlags(HOBJECT hObj) override { LTSTUB("ILTServer::GetObjectFlags"); return (uint32)0; }
    void SetObjectFlags(HOBJECT hObj, uint32 flags) override { LTSTUB("ILTServer::SetObjectFlags"); }
    LTRESULT GetNetFlags(HOBJECT hObj, uint32 &flags) override { LTSTUB("ILTServer::GetNetFlags"); return LT_ERROR; }
    LTRESULT SetNetFlags(HOBJECT hObj, uint32 flags) override { LTSTUB("ILTServer::SetNetFlags"); return LT_ERROR; }
    void GetObjectDims(HOBJECT hObj, LTVector *pNewDims) override { LTSTUB("ILTServer::GetObjectDims"); }
    LTRESULT SetObjectDims(HOBJECT hObj, LTVector *pNewDims) override { LTSTUB("ILTServer::SetObjectDims"); return LT_ERROR; }
    LTRESULT SetObjectDims2(HOBJECT hObj, LTVector *pNewDims) override { LTSTUB("ILTServer::SetObjectDims2"); return LT_ERROR; }
    LTRESULT GetVelocity(HOBJECT hObj, LTVector *pVel) override { LTSTUB("ILTServer::GetVelocity"); return LT_ERROR; }
    LTRESULT SetVelocity(HOBJECT hObj, LTVector *pVel) override { LTSTUB("ILTServer::SetVelocity"); return LT_ERROR; }
    LTRESULT GetAcceleration(HOBJECT hObj, LTVector *pAccel) override { LTSTUB("ILTServer::GetAcceleration"); return LT_ERROR; }
    LTRESULT SetAcceleration(HOBJECT hObj, LTVector *pAccel) override { LTSTUB("ILTServer::SetAcceleration"); return LT_ERROR; }
    LTRESULT SetModelFilenames(HOBJECT hObj, char *pFilename, char *pSkinName) override { LTSTUB("ILTServer::SetModelFilenames"); return LT_ERROR; }
    LTRESULT SetObjectFilenames(HOBJECT hObj, char *pFilename, char *pSkinName) override { LTSTUB("ILTServer::SetObjectFilenames"); return LT_ERROR; }
    LTRESULT GetModelAnimUserDims(HOBJECT hObj, LTVector *pDims, HMODELANIM hAnim) override { LTSTUB("ILTServer::GetModelAnimUserDims"); return LT_ERROR; }
    LTRESULT GetPolyTextureFlags(HPOLY hPoly, uint32 *pFlags) override { LTSTUB("ILTServer::GetPolyTextureFlags"); return LT_ERROR; }
    LTRESULT GetClientData(HCLIENT hClient, void *&pData, uint32 &nLength) override { LTSTUB("ILTServer::GetClientData"); return LT_ERROR; }
    HMESSAGEWRITE StartHMessageWrite() override { LTSTUB("ILTCSBase::StartHMessageWrite"); return (HMESSAGEWRITE)0; }
    HMODELANIM GetAnimIndex(HOBJECT hObj, char *pAnimName) override { LTSTUB("ILTCSBase::GetAnimIndex"); return (HMODELANIM)0; }
    void SetModelAnimation(HOBJECT hObj, HMODELANIM hAnim) override { LTSTUB("ILTCSBase::SetModelAnimation"); }
    HMODELANIM GetModelAnimation(HOBJECT hObj) override { LTSTUB("ILTCSBase::GetModelAnimation"); return (HMODELANIM)0; }
    void SetModelLooping(HOBJECT hObj, LTBOOL bLoop) override { LTSTUB("ILTCSBase::SetModelLooping"); }
    LTBOOL GetModelLooping(HOBJECT hObj) override { LTSTUB("ILTCSBase::GetModelLooping"); return LTFALSE; }
    LTRESULT ResetModelAnimation(HOBJECT hObj) override { LTSTUB("ILTCSBase::ResetModelAnimation"); return LT_ERROR; }
    uint32 GetModelPlaybackState(HOBJECT hObj) override { LTSTUB("ILTCSBase::GetModelPlaybackState"); return (uint32)0; }
    LTRESULT FreeUnusedModels() override { LTSTUB("ILTCSBase::FreeUnusedModels"); return LT_ERROR; }
    void CPrint(char *pMsg, ...) override { LTSTUB("ILTCSBase::CPrint"); }
    uint32 GetPointContainers(LTVector *pPoint, HOBJECT *pList, uint32 maxListSize) override { LTSTUB("ILTCSBase::GetPointContainers"); return (uint32)0; }
    LTBOOL GetContainerCode(HOBJECT hObj, uint16 *pCode) override { LTSTUB("ILTCSBase::GetContainerCode"); return LTFALSE; }
    LTRESULT OpenFile(char *pFilename, ILTStream **pStream) override { LTSTUB("ILTCSBase::OpenFile"); return LT_ERROR; }
    LTRESULT CopyFile(const char *pszSourceFile, const char *pszDestFile) override { LTSTUB("ILTCSBase::CopyFile"); return LT_ERROR; }
    HSTRING FormatString(int messageCode, ...) override { LTSTUB("ILTCSBase::FormatString"); return (HSTRING)0; }
    HSTRING CopyString(HSTRING hString) override { LTSTUB("ILTCSBase::CopyString"); return (HSTRING)0; }
    HSTRING CreateString(char *pString) override { LTSTUB("ILTCSBase::CreateString"); return (HSTRING)0; }
    void FreeString(HSTRING hString) override { LTSTUB("ILTCSBase::FreeString"); }
    LTBOOL CompareStrings(HSTRING hString1, HSTRING hString2) override { LTSTUB("ILTCSBase::CompareStrings"); return LTFALSE; }
    LTBOOL CompareStringsUpper(HSTRING hString1, HSTRING hString2) override { LTSTUB("ILTCSBase::CompareStringsUpper"); return LTFALSE; }
    char* GetStringData(HSTRING hString) override { LTSTUB("ILTCSBase::GetStringData"); return 0; }
    float GetVarValueFloat(HCONSOLEVAR hVar) override { LTSTUB("ILTCSBase::GetVarValueFloat"); return 0.0f; }
    char* GetVarValueString(HCONSOLEVAR hVar) override { LTSTUB("ILTCSBase::GetVarValueString"); return 0; }
    LTFLOAT GetTime() override { LTSTUB("ILTCSBase::GetTime"); return 0.0f; }
    LTFLOAT GetFrameTime() override { LTSTUB("ILTCSBase::GetFrameTime"); return 0.0f; }
    LTRESULT RemoveObject(HOBJECT hObj) override { LTSTUB("ILTCSBase::RemoveObject"); return LT_ERROR; }
    LTRESULT GetRotationVectors(LTRotation *pRotation, LTVector *pUp, LTVector *pRight, LTVector *pForward) override { LTSTUB("ILTCSBase::GetRotationVectors"); return LT_ERROR; }
    void GetRotationVectorsFromMatrix(LTMatrix *pMat, LTVector *pUp, LTVector *pRight, LTVector *pForward) override { LTSTUB("ILTCSBase::GetRotationVectorsFromMatrix"); }
    void RotateAroundAxis(LTRotation *pRotation, LTVector *pAxis, float amount) override { LTSTUB("ILTCSBase::RotateAroundAxis"); }
    void EulerRotateX(LTRotation *pRotation, float amount) override { LTSTUB("ILTCSBase::EulerRotateX"); }
    void EulerRotateY(LTRotation *pRotation, float amount) override { LTSTUB("ILTCSBase::EulerRotateY"); }
    void EulerRotateZ(LTRotation *pRotation, float amount) override { LTSTUB("ILTCSBase::EulerRotateZ"); }
    void AlignRotation(LTRotation *pRotation, LTVector *pVector, LTVector *pUp) override { LTSTUB("ILTCSBase::AlignRotation"); }
    LTRESULT SetupEuler(LTRotation *pRotation, float pitch, float yaw, float roll) override { LTSTUB("ILTCSBase::SetupEuler"); return LT_ERROR; }
    LTRESULT InterpolateRotation(LTRotation *pDest, LTRotation *pRot1, LTRotation *pRot2, float t) override { LTSTUB("ILTCSBase::InterpolateRotation"); return LT_ERROR; }
    void SetupTransformationMatrix(LTMatrix *pMat, LTVector *pTranslation, LTRotation *pRotation) override { LTSTUB("ILTCSBase::SetupTransformationMatrix"); }
    void SetupTranslationMatrix(LTMatrix *pMat, LTVector *pTranslation) override { LTSTUB("ILTCSBase::SetupTranslationMatrix"); }
    void SetupRotationMatrix(LTMatrix *pMat, LTRotation *pRot) override { LTSTUB("ILTCSBase::SetupRotationMatrix"); }
    void SetupTranslationFromMatrix(LTVector *pTranslation, LTMatrix *pMat) override { LTSTUB("ILTCSBase::SetupTranslationFromMatrix"); }
    void SetupRotationFromMatrix(LTRotation *pRot, LTMatrix *pMat) override { LTSTUB("ILTCSBase::SetupRotationFromMatrix"); }
    void SetupRotationAroundPoint(LTMatrix *pMat, LTRotation *pRot, LTVector *pPoint) override { LTSTUB("ILTCSBase::SetupRotationAroundPoint"); }

    // Sub-interface DATA members. ⚠️ Not methods and not
    // function pointers -- reached through INLINE accessors, so
    // nothing here ever passes through a stub. Left NULL they are
    // a null read with no [LTSTUB] line before it.
    //
    // Call this from the most-derived constructor, BEFORE any
    // deliberate per-member override: the last assignment wins, and
    // the host has reasons to prefer some variants (the CLIENT
    // flavours of ILTPhysics and ILTSoundMgr, for instance).
    //
    // ⚠️ DECLARED here, DEFINED at the bottom of this header. The
    // stub types it assigns are declared LATER in the file, and an
    // inline body would not see them: a member function body is
    // parsed in its own class's complete-class context, which
    // delays lookup for THAT class's members only, not for types
    // that appear further down.
    void LTSkel_WireData();
};

// LTStub_ILTCommon -- from iltcommon.h
class LTStub_ILTCommon : public ILTCommon
{
public:
    LTRESULT SetObjectFilenames(HOBJECT pObj, ObjectCreateStruct *pStruct) override { LTSTUB("ILTCommon::SetObjectFilenames"); return LT_ERROR; }
    LTRESULT GetObjectFlags(const HOBJECT hObj, const ObjFlagType flagType, uint32 &dwFlags) override { LTSTUB("ILTCommon::GetObjectFlags"); return LT_ERROR; }
    LTRESULT SetObjectFlags(HOBJECT hObj, const ObjFlagType flagType, uint32 dwFlags) override { LTSTUB("ILTCommon::SetObjectFlags"); return LT_ERROR; }
    LTRESULT GetAttachmentObjects(HATTACHMENT hAttachment, HOBJECT &hParent, HOBJECT &hChild) override { LTSTUB("ILTCommon::GetAttachmentObjects"); return LT_ERROR; }
    LTRESULT GetAttachments(HLOCALOBJ hObj, HLOCALOBJ *inList, uint32 inListSize, uint32 &outListSize, uint32 &outNumAttachments) override { LTSTUB("ILTCommon::GetAttachments"); return LT_ERROR; }
    LTRESULT GetAttachmentTransform(HATTACHMENT hAttachment, LTransform &transform, LTBOOL bWorldSpace) override { LTSTUB("ILTCommon::GetAttachmentTransform"); return LT_ERROR; }
    LTRESULT GetAttachedModelNodeTransform(HATTACHMENT hAttachment, HMODELNODE hNode, LTransform &transform) override { LTSTUB("ILTCommon::GetAttachedModelNodeTransform"); return LT_ERROR; }
    LTRESULT GetAttachedModelSocketTransform(HATTACHMENT hAttachment, HMODELSOCKET hSocket, LTransform &transform) override { LTSTUB("ILTCommon::GetAttachedModelSocketTransform"); return LT_ERROR; }
    LTRESULT Parse(ConParse *pParse) override { LTSTUB("ILTCommon::Parse"); return LT_ERROR; }
    LTRESULT GetObjectType(HOBJECT hObj, uint32 *type) override { LTSTUB("ILTCommon::GetObjectType"); return LT_ERROR; }
    LTRESULT GetModelAnimUserDims(HOBJECT hObject, LTVector *pDims, HMODELANIM hAnim) override { LTSTUB("ILTCommon::GetModelAnimUserDims"); return LT_ERROR; }
    LTRESULT GetRotationVectors(LTRotation &rot, LTVector &up, LTVector &right, LTVector &forward) override { LTSTUB("ILTCommon::GetRotationVectors"); return LT_ERROR; }
    LTRESULT SetupEuler(LTRotation &rot, float pitch, float yaw, float roll) override { LTSTUB("ILTCommon::SetupEuler"); return LT_ERROR; }
    LTRESULT CreateMessage(ILTMessage* &pMsg) override { LTSTUB("ILTCommon::CreateMessage"); return LT_ERROR; }
    LTRESULT GetPointStatus(LTVector *pPoint) override { LTSTUB("ILTCommon::GetPointStatus"); return LT_ERROR; }
    LTRESULT GetPointShade(LTVector *pPoint, LTVector *pColor) override { LTSTUB("ILTCommon::GetPointShade"); return LT_ERROR; }
    LTRESULT GetPolyTextureFlags(HPOLY hPoly, uint32 *pFlags) override { LTSTUB("ILTCommon::GetPolyTextureFlags"); return LT_ERROR; }

    // Sub-interface DATA members. ⚠️ Not methods and not
    // function pointers -- reached through INLINE accessors, so
    // nothing here ever passes through a stub. Left NULL they are
    // a null read with no [LTSTUB] line before it.
    //
    // Call this from the most-derived constructor, BEFORE any
    // deliberate per-member override: the last assignment wins, and
    // the host has reasons to prefer some variants (the CLIENT
    // flavours of ILTPhysics and ILTSoundMgr, for instance).
    //
    // ⚠️ DECLARED here, DEFINED at the bottom of this header. The
    // stub types it assigns are declared LATER in the file, and an
    // inline body would not see them: a member function body is
    // parsed in its own class's complete-class context, which
    // delays lookup for THAT class's members only, not for types
    // that appear further down.
    void LTSkel_WireData();
};

// LTStub_ILTModel -- from iltmodel.h
class LTStub_ILTModel : public ILTModel
{
public:
    LTRESULT GetSocket(HOBJECT hObj, char *pSocketName, HMODELSOCKET &hSocket) override { LTSTUB("ILTModel::GetSocket"); return LT_ERROR; }
    LTRESULT GetNode(HOBJECT hObj, char *pNodeName, HMODELNODE &hNode) override { LTSTUB("ILTModel::GetNode"); return LT_ERROR; }
    LTRESULT GetPiece(HOBJECT hObj, char *pPieceName, HMODELPIECE &hPiece) override { LTSTUB("ILTModel::GetPiece"); return LT_ERROR; }
    LTRESULT FindWeightSet(HOBJECT hObj, char *pSetName, HMODELWEIGHTSET &hSet) override { LTSTUB("ILTModel::FindWeightSet"); return LT_ERROR; }
    LTRESULT GetPieceHideStatus(HOBJECT hObj, HMODELPIECE hPiece, LTBOOL &bHidden) override { LTSTUB("ILTModel::GetPieceHideStatus"); return LT_ERROR; }
    LTRESULT SetPieceHideStatus(HOBJECT hObj, HMODELPIECE hPiece, LTBOOL bHidden) override { LTSTUB("ILTModel::SetPieceHideStatus"); return LT_ERROR; }
    LTRESULT GetNodeTransform(HOBJECT hObj, HMODELNODE hNode, LTransform &transform, LTBOOL bWorldSpace) override { LTSTUB("ILTModel::GetNodeTransform"); return LT_ERROR; }
    LTRESULT GetSocketTransform(HOBJECT hObj, HMODELSOCKET hSocket, LTransform &transform, LTBOOL bWorldSpace) override { LTSTUB("ILTModel::GetSocketTransform"); return LT_ERROR; }
    LTRESULT GetInstanceHookFn(HOBJECT hObj, ModelInstanceHookFn &fn) override { LTSTUB("ILTModel::GetInstanceHookFn"); return LT_ERROR; }
    LTRESULT SetInstanceHookFn(HOBJECT hObj, ModelInstanceHookFn fn) override { LTSTUB("ILTModel::SetInstanceHookFn"); return LT_ERROR; }
    LTRESULT GetMainTracker(HOBJECT hObj, LTAnimTracker* &pTracker) override { LTSTUB("ILTModel::GetMainTracker"); return LT_ERROR; }
    LTRESULT UpdateMainTracker(HOBJECT pObj, LTFLOAT fUpdateDelta) override { LTSTUB("ILTModel::UpdateMainTracker"); return LT_ERROR; }
    LTRESULT GetPlaybackState(LTAnimTracker *pTracker, uint32 &flags) override { LTSTUB("ILTModel::GetPlaybackState"); return LT_ERROR; }
    LTRESULT AddTracker(HOBJECT hObj, LTAnimTracker *pTracker) override { LTSTUB("ILTModel::AddTracker"); return LT_ERROR; }
    LTRESULT RemoveTracker(HOBJECT hObj, LTAnimTracker *pTracker) override { LTSTUB("ILTModel::RemoveTracker"); return LT_ERROR; }
    LTRESULT GetCurAnim(LTAnimTracker *pTracker, HMODELANIM &hAnim) override { LTSTUB("ILTModel::GetCurAnim"); return LT_ERROR; }
    LTRESULT SetCurAnim(LTAnimTracker *pTracker, HMODELANIM hAnim) override { LTSTUB("ILTModel::SetCurAnim"); return LT_ERROR; }
    LTRESULT ResetAnim(LTAnimTracker *pTracker) override { LTSTUB("ILTModel::ResetAnim"); return LT_ERROR; }
    LTRESULT GetLooping(LTAnimTracker *pTracker) override { LTSTUB("ILTModel::GetLooping"); return LT_ERROR; }
    LTRESULT SetLooping(LTAnimTracker *pTracker, LTBOOL bLooping) override { LTSTUB("ILTModel::SetLooping"); return LT_ERROR; }
    LTRESULT GetPlaying(LTAnimTracker *pTracker) override { LTSTUB("ILTModel::GetPlaying"); return LT_ERROR; }
    LTRESULT SetPlaying(LTAnimTracker *pTracker, LTBOOL bPlaying) override { LTSTUB("ILTModel::SetPlaying"); return LT_ERROR; }
    LTRESULT GetWeightSet(LTAnimTracker *pTracker, HMODELWEIGHTSET &hSet) override { LTSTUB("ILTModel::GetWeightSet"); return LT_ERROR; }
    LTRESULT SetWeightSet(LTAnimTracker *pTracker, HMODELWEIGHTSET hSet) override { LTSTUB("ILTModel::SetWeightSet"); return LT_ERROR; }
    LTRESULT GetCurAnimLength(LTAnimTracker *pTracker, uint32 &length) override { LTSTUB("ILTModel::GetCurAnimLength"); return LT_ERROR; }
    LTRESULT GetCurAnimTime(LTAnimTracker *pTracker, uint32 &curTime) override { LTSTUB("ILTModel::GetCurAnimTime"); return LT_ERROR; }
    LTRESULT SetCurAnimTime(LTAnimTracker *pTracker, uint32 curTime) override { LTSTUB("ILTModel::SetCurAnimTime"); return LT_ERROR; }
};

// LTStub_ILTPhysics -- from iltphysics.h
class LTStub_ILTPhysics : public ILTPhysics
{
public:
    LTRESULT GetFrictionCoefficient(HOBJECT hObj, float &coeff) override { LTSTUB("ILTPhysics::GetFrictionCoefficient"); return LT_ERROR; }
    LTRESULT SetFrictionCoefficient(HOBJECT hObj, float coeff) override { LTSTUB("ILTPhysics::SetFrictionCoefficient"); return LT_ERROR; }
    LTRESULT GetForceIgnoreLimit(HOBJECT hObj, float &limit) override { LTSTUB("ILTPhysics::GetForceIgnoreLimit"); return LT_ERROR; }
    LTRESULT SetForceIgnoreLimit(HOBJECT hObj, float limit) override { LTSTUB("ILTPhysics::SetForceIgnoreLimit"); return LT_ERROR; }
    LTRESULT GetVelocity(HOBJECT hObj, LTVector *pVel) override { LTSTUB("ILTPhysics::GetVelocity"); return LT_ERROR; }
    LTRESULT SetVelocity(HOBJECT hObj, LTVector *pVel) override { LTSTUB("ILTPhysics::SetVelocity"); return LT_ERROR; }
    LTRESULT GetAcceleration(HOBJECT hObj, LTVector *pAccel) override { LTSTUB("ILTPhysics::GetAcceleration"); return LT_ERROR; }
    LTRESULT SetAcceleration(HOBJECT hObj, LTVector *pAccel) override { LTSTUB("ILTPhysics::SetAcceleration"); return LT_ERROR; }
    LTRESULT GetObjectMass(HOBJECT hObj, float &mass) override { LTSTUB("ILTPhysics::GetObjectMass"); return LT_ERROR; }
    LTRESULT SetObjectMass(HOBJECT hObj, float mass) override { LTSTUB("ILTPhysics::SetObjectMass"); return LT_ERROR; }
    LTRESULT GetObjectDims(HOBJECT hObj, LTVector *pNewDims) override { LTSTUB("ILTPhysics::GetObjectDims"); return LT_ERROR; }
    LTRESULT SetObjectDims(HOBJECT hObj, LTVector *pNewDims, uint32 flags) override { LTSTUB("ILTPhysics::SetObjectDims"); return LT_ERROR; }
    LTRESULT MoveObject(HOBJECT hObj, LTVector *pPos, uint32 flags) override { LTSTUB("ILTPhysics::MoveObject"); return LT_ERROR; }
    LTRESULT GetStandingOn(HOBJECT hObj, CollisionInfo *pInfo) override { LTSTUB("ILTPhysics::GetStandingOn"); return LT_ERROR; }
    LTRESULT IsWorldObject(HOBJECT hObj) override { LTSTUB("ILTPhysics::IsWorldObject"); return LT_ERROR; }
    LTRESULT GetGlobalForce(LTVector &vec) override { LTSTUB("ILTPhysics::GetGlobalForce"); return LT_ERROR; }
    LTRESULT SetGlobalForce(LTVector &vec) override { LTSTUB("ILTPhysics::SetGlobalForce"); return LT_ERROR; }
    LTRESULT GetStairHeight(float &fHeight) override { LTSTUB("ILTPhysics::GetStairHeight"); return LT_ERROR; }
    LTRESULT SetStairHeight(float fHeight) override { LTSTUB("ILTPhysics::SetStairHeight"); return LT_ERROR; }
};

// LTStub_ILTClientPhysics -- from iltphysics.h
class LTStub_ILTClientPhysics : public ILTClientPhysics
{
public:
    LTRESULT UpdateMovement(MoveInfo *pInfo) override { LTSTUB("ILTClientPhysics::UpdateMovement"); return LT_ERROR; }
    LTRESULT MovePushObjects(HOBJECT hToMove, LTVector &newPos, HOBJECT *hPushObjects, uint32 nPushObjects) override { LTSTUB("ILTClientPhysics::MovePushObjects"); return LT_ERROR; }
    LTRESULT RotatePushObjects(HOBJECT hToMove, LTRotation &newRot, HOBJECT *hPushObjects, uint32 nPushObjects) override { LTSTUB("ILTClientPhysics::RotatePushObjects"); return LT_ERROR; }
    LTRESULT GetFrictionCoefficient(HOBJECT hObj, float &coeff) override { LTSTUB("ILTPhysics::GetFrictionCoefficient"); return LT_ERROR; }
    LTRESULT SetFrictionCoefficient(HOBJECT hObj, float coeff) override { LTSTUB("ILTPhysics::SetFrictionCoefficient"); return LT_ERROR; }
    LTRESULT GetForceIgnoreLimit(HOBJECT hObj, float &limit) override { LTSTUB("ILTPhysics::GetForceIgnoreLimit"); return LT_ERROR; }
    LTRESULT SetForceIgnoreLimit(HOBJECT hObj, float limit) override { LTSTUB("ILTPhysics::SetForceIgnoreLimit"); return LT_ERROR; }
    LTRESULT GetVelocity(HOBJECT hObj, LTVector *pVel) override { LTSTUB("ILTPhysics::GetVelocity"); return LT_ERROR; }
    LTRESULT SetVelocity(HOBJECT hObj, LTVector *pVel) override { LTSTUB("ILTPhysics::SetVelocity"); return LT_ERROR; }
    LTRESULT GetAcceleration(HOBJECT hObj, LTVector *pAccel) override { LTSTUB("ILTPhysics::GetAcceleration"); return LT_ERROR; }
    LTRESULT SetAcceleration(HOBJECT hObj, LTVector *pAccel) override { LTSTUB("ILTPhysics::SetAcceleration"); return LT_ERROR; }
    LTRESULT GetObjectMass(HOBJECT hObj, float &mass) override { LTSTUB("ILTPhysics::GetObjectMass"); return LT_ERROR; }
    LTRESULT SetObjectMass(HOBJECT hObj, float mass) override { LTSTUB("ILTPhysics::SetObjectMass"); return LT_ERROR; }
    LTRESULT GetObjectDims(HOBJECT hObj, LTVector *pNewDims) override { LTSTUB("ILTPhysics::GetObjectDims"); return LT_ERROR; }
    LTRESULT SetObjectDims(HOBJECT hObj, LTVector *pNewDims, uint32 flags) override { LTSTUB("ILTPhysics::SetObjectDims"); return LT_ERROR; }
    LTRESULT MoveObject(HOBJECT hObj, LTVector *pPos, uint32 flags) override { LTSTUB("ILTPhysics::MoveObject"); return LT_ERROR; }
    LTRESULT GetStandingOn(HOBJECT hObj, CollisionInfo *pInfo) override { LTSTUB("ILTPhysics::GetStandingOn"); return LT_ERROR; }
    LTRESULT IsWorldObject(HOBJECT hObj) override { LTSTUB("ILTPhysics::IsWorldObject"); return LT_ERROR; }
    LTRESULT GetGlobalForce(LTVector &vec) override { LTSTUB("ILTPhysics::GetGlobalForce"); return LT_ERROR; }
    LTRESULT SetGlobalForce(LTVector &vec) override { LTSTUB("ILTPhysics::SetGlobalForce"); return LT_ERROR; }
    LTRESULT GetStairHeight(float &fHeight) override { LTSTUB("ILTPhysics::GetStairHeight"); return LT_ERROR; }
    LTRESULT SetStairHeight(float fHeight) override { LTSTUB("ILTPhysics::SetStairHeight"); return LT_ERROR; }
};

// LTStub_ILTSoundMgr -- from iltsoundmgr.h
class LTStub_ILTSoundMgr : public ILTSoundMgr
{
public:
    LTRESULT PlaySound(PlaySoundInfo *pPlaySoundInfo, HLTSOUND &hResult) override { LTSTUB("ILTSoundMgr::PlaySound"); return LT_ERROR; }
    LTRESULT GetSoundDuration(HLTSOUND hSound, LTFLOAT &fDuration) override { LTSTUB("ILTSoundMgr::GetSoundDuration"); return LT_ERROR; }
    LTRESULT IsSoundDone(HLTSOUND hSound, LTBOOL &bDone) override { LTSTUB("ILTSoundMgr::IsSoundDone"); return LT_ERROR; }
    LTRESULT KillSound(HLTSOUND hSound) override { LTSTUB("ILTSoundMgr::KillSound"); return LT_ERROR; }
    LTRESULT KillSoundLoop(HLTSOUND hSound) override { LTSTUB("ILTSoundMgr::KillSoundLoop"); return LT_ERROR; }
};

// LTStub_ILTClientSoundMgr -- from iltsoundmgr.h
class LTStub_ILTClientSoundMgr : public ILTClientSoundMgr
{
public:
    LTRESULT GetSound3DProviderLists(Sound3DProvider *&pSound3DProviderList, LTBOOL bVerify) override { LTSTUB("ILTClientSoundMgr::GetSound3DProviderLists"); return LT_ERROR; }
    LTRESULT ReleaseSound3DProviderList(Sound3DProvider *pSound3DProviderList) override { LTSTUB("ILTClientSoundMgr::ReleaseSound3DProviderList"); return LT_ERROR; }
    LTRESULT InitSound(InitSoundInfo *pSoundInfo) override { LTSTUB("ILTClientSoundMgr::InitSound"); return LT_ERROR; }
    LTRESULT GetVolume(uint16 &nVolume) override { LTSTUB("ILTClientSoundMgr::GetVolume"); return LT_ERROR; }
    LTRESULT SetVolume(uint16 nVolume) override { LTSTUB("ILTClientSoundMgr::SetVolume"); return LT_ERROR; }
    LTRESULT SetReverbProperties(ReverbProperties *pReverbProperties) override { LTSTUB("ILTClientSoundMgr::SetReverbProperties"); return LT_ERROR; }
    LTRESULT GetReverbProperties(ReverbProperties *pReverbProperties) override { LTSTUB("ILTClientSoundMgr::GetReverbProperties"); return LT_ERROR; }
    LTRESULT SetSoundOcclusion(HLTSOUND hSound, LTFLOAT fLevel) override { LTSTUB("ILTClientSoundMgr::SetSoundOcclusion"); return LT_ERROR; }
    LTRESULT GetSoundOcclusion(HLTSOUND hSound, LTFLOAT *pLevel) override { LTSTUB("ILTClientSoundMgr::GetSoundOcclusion"); return LT_ERROR; }
    LTRESULT SetSoundObstruction(HLTSOUND hSound, LTFLOAT fLevel) override { LTSTUB("ILTClientSoundMgr::SetSoundObstruction"); return LT_ERROR; }
    LTRESULT GetSoundObstruction(HLTSOUND hSound, LTFLOAT *pLevel) override { LTSTUB("ILTClientSoundMgr::GetSoundObstruction"); return LT_ERROR; }
    LTRESULT SetSoundFilter(HLTSOUND hSound, const char *pFilter) override { LTSTUB("ILTClientSoundMgr::SetSoundFilter"); return LT_ERROR; }
    LTRESULT SetSoundFilterParam(HLTSOUND hSound, const char *pParam, float fValue) override { LTSTUB("ILTClientSoundMgr::SetSoundFilterParam"); return LT_ERROR; }
    LTRESULT GetSoundFilterParam(HLTSOUND hSound, const char *pParam, float *pValue) override { LTSTUB("ILTClientSoundMgr::GetSoundFilterParam"); return LT_ERROR; }
    LTRESULT GetFilterName(uint32 nIndex, const char **pFilter) override { LTSTUB("ILTClientSoundMgr::GetFilterName"); return LT_ERROR; }
    LTRESULT GetFilterParamName(uint32 nIndex, const char *pFilter, const char **pParam) override { LTSTUB("ILTClientSoundMgr::GetFilterParamName"); return LT_ERROR; }
    LTRESULT SetSoundPosition(HLTSOUND hSound, LTVector *pPos) override { LTSTUB("ILTClientSoundMgr::SetSoundPosition"); return LT_ERROR; }
    LTRESULT GetSoundPosition(HLTSOUND hSound, LTVector *pPos) override { LTSTUB("ILTClientSoundMgr::GetSoundPosition"); return LT_ERROR; }
    LTRESULT PauseSounds() override { LTSTUB("ILTClientSoundMgr::PauseSounds"); return LT_ERROR; }
    LTRESULT ResumeSounds() override { LTSTUB("ILTClientSoundMgr::ResumeSounds"); return LT_ERROR; }
    LTRESULT SetListener(LTBOOL bListenerInClient, LTVector *pPos, LTRotation *pRot, LTBOOL bTeleport) override { LTSTUB("ILTClientSoundMgr::SetListener"); return LT_ERROR; }
    LTRESULT ReleaseSoundHandle(HLTSOUND hSound) override { LTSTUB("ILTClientSoundMgr::ReleaseSoundHandle"); return LT_ERROR; }
    LTRESULT PlaySound(PlaySoundInfo *pPlaySoundInfo, HLTSOUND &hResult) override { LTSTUB("ILTSoundMgr::PlaySound"); return LT_ERROR; }
    LTRESULT GetSoundDuration(HLTSOUND hSound, LTFLOAT &fDuration) override { LTSTUB("ILTSoundMgr::GetSoundDuration"); return LT_ERROR; }
    LTRESULT IsSoundDone(HLTSOUND hSound, LTBOOL &bDone) override { LTSTUB("ILTSoundMgr::IsSoundDone"); return LT_ERROR; }
    LTRESULT KillSound(HLTSOUND hSound) override { LTSTUB("ILTSoundMgr::KillSound"); return LT_ERROR; }
    LTRESULT KillSoundLoop(HLTSOUND hSound) override { LTSTUB("ILTSoundMgr::KillSoundLoop"); return LT_ERROR; }
};

// LTStub_ILTTransform -- from ilttransform.h
class LTStub_ILTTransform : public ILTTransform
{
public:
    LTRESULT Get(LTransform &transform, LTVector &pos, LTRotation &rot) override { LTSTUB("ILTTransform::Get"); return LT_ERROR; }
    LTRESULT Set(LTransform &transform, LTVector &pos, LTRotation &rot) override { LTSTUB("ILTTransform::Set"); return LT_ERROR; }
    LTRESULT GetPos(LTransform &transform, LTVector &pos) override { LTSTUB("ILTTransform::GetPos"); return LT_ERROR; }
    LTRESULT GetRot(LTransform &transform, LTRotation &rot) override { LTSTUB("ILTTransform::GetRot"); return LT_ERROR; }
    LTRESULT ToMatrix(LTransform &transform, LTMatrix &mat) override { LTSTUB("ILTTransform::ToMatrix"); return LT_ERROR; }
    LTRESULT FromMatrix(LTransform &transform, LTMatrix &mat) override { LTSTUB("ILTTransform::FromMatrix"); return LT_ERROR; }
    LTRESULT Inverse(LTransform &inverse, LTransform &transform) override { LTSTUB("ILTTransform::Inverse"); return LT_ERROR; }
    LTRESULT Multiply(LTransform &out, LTransform &t1, LTransform &t2) override { LTSTUB("ILTTransform::Multiply"); return LT_ERROR; }
    LTRESULT Difference(LTransform &diff, LTransform &t1, LTransform &t2) override { LTSTUB("ILTTransform::Difference"); return LT_ERROR; }
};

// LTStub_ILTMath -- from iltmath.h
class LTStub_ILTMath : public ILTMath
{
public:
};

// LTStub_ILTMessage -- from iltmessage.h
class LTStub_ILTMessage : public ILTMessage
{
public:
    LTRESULT Release() override { LTSTUB("ILTMessage::Release"); return LT_ERROR; }
    LTRESULT ReadByteFL(uint8 &val) override { LTSTUB("ILTMessage::ReadByteFL"); return LT_ERROR; }
    LTRESULT ReadWordFL(uint16 &val) override { LTSTUB("ILTMessage::ReadWordFL"); return LT_ERROR; }
    LTRESULT ReadDWordFL(uint32 &val) override { LTSTUB("ILTMessage::ReadDWordFL"); return LT_ERROR; }
    LTRESULT ReadFloatFL(float &val) override { LTSTUB("ILTMessage::ReadFloatFL"); return LT_ERROR; }
    LTRESULT ReadStringFL(char *pData, uint32 maxBytes) override { LTSTUB("ILTMessage::ReadStringFL"); return LT_ERROR; }
    LTRESULT ReadHStringFL(HSTRING &hString) override { LTSTUB("ILTMessage::ReadHStringFL"); return LT_ERROR; }
    LTRESULT ReadHStringAsStringFL(char *pMsg, uint32 msgBufSize) override { LTSTUB("ILTMessage::ReadHStringAsStringFL"); return LT_ERROR; }
    LTRESULT ReadRawFL(void *pData, uint32 len) override { LTSTUB("ILTMessage::ReadRawFL"); return LT_ERROR; }
    LTRESULT ReadVectorFL(LTVector &vec) override { LTSTUB("ILTMessage::ReadVectorFL"); return LT_ERROR; }
    LTRESULT ReadCompVectorFL(LTVector &vec) override { LTSTUB("ILTMessage::ReadCompVectorFL"); return LT_ERROR; }
    LTRESULT ReadCompPosFL(LTVector &vec) override { LTSTUB("ILTMessage::ReadCompPosFL"); return LT_ERROR; }
    LTRESULT ReadRotationFL(LTRotation &rot) override { LTSTUB("ILTMessage::ReadRotationFL"); return LT_ERROR; }
    LTRESULT ReadCompRotationFL(LTRotation &rot) override { LTSTUB("ILTMessage::ReadCompRotationFL"); return LT_ERROR; }
    LTRESULT ReadMessageFL(ILTMessage* &pMsg) override { LTSTUB("ILTMessage::ReadMessageFL"); return LT_ERROR; }
    LTRESULT ReadObjectFL(HOBJECT &hObj) override { LTSTUB("ILTMessage::ReadObjectFL"); return LT_ERROR; }
    LTRESULT WriteByte(uint8 val) override { LTSTUB("ILTMessage::WriteByte"); return LT_ERROR; }
    LTRESULT WriteWord(uint16 val) override { LTSTUB("ILTMessage::WriteWord"); return LT_ERROR; }
    LTRESULT WriteDWord(uint32 val) override { LTSTUB("ILTMessage::WriteDWord"); return LT_ERROR; }
    LTRESULT WriteFloat(float val) override { LTSTUB("ILTMessage::WriteFloat"); return LT_ERROR; }
    LTRESULT WriteString(char *pData) override { LTSTUB("ILTMessage::WriteString"); return LT_ERROR; }
    LTRESULT WriteVector(LTVector &vec) override { LTSTUB("ILTMessage::WriteVector"); return LT_ERROR; }
    LTRESULT WriteCompVector(LTVector &vec) override { LTSTUB("ILTMessage::WriteCompVector"); return LT_ERROR; }
    LTRESULT WriteCompPos(LTVector &vec) override { LTSTUB("ILTMessage::WriteCompPos"); return LT_ERROR; }
    LTRESULT WriteRotation(LTRotation &rot) override { LTSTUB("ILTMessage::WriteRotation"); return LT_ERROR; }
    LTRESULT WriteCompRotation(LTRotation &rot) override { LTSTUB("ILTMessage::WriteCompRotation"); return LT_ERROR; }
    LTRESULT WriteRaw(void *pData, uint32 len) override { LTSTUB("ILTMessage::WriteRaw"); return LT_ERROR; }
    LTRESULT WriteMessage(ILTMessage &msg) override { LTSTUB("ILTMessage::WriteMessage"); return LT_ERROR; }
    LTRESULT WriteHString(HSTRING hString) override { LTSTUB("ILTMessage::WriteHString"); return LT_ERROR; }
    LTRESULT WriteHStringFormatted(int messageCode, ...) override { LTSTUB("ILTMessage::WriteHStringFormatted"); return LT_ERROR; }
    LTRESULT WriteHStringArgList(int messageCode, va_list *pList) override { LTSTUB("ILTMessage::WriteHStringArgList"); return LT_ERROR; }
    LTRESULT WriteStringAsHString(char *pStr) override { LTSTUB("ILTMessage::WriteStringAsHString"); return LT_ERROR; }
    LTRESULT WriteObject(HOBJECT hObj) override { LTSTUB("ILTMessage::WriteObject"); return LT_ERROR; }
    LTRESULT ResetPos() override { LTSTUB("ILTMessage::ResetPos"); return LT_ERROR; }
    LTRESULT GetStatus(uint32 &flags) override { LTSTUB("ILTMessage::GetStatus"); return LT_ERROR; }
};

// LTStub_ILTCursorInst -- from iltcursor.h
class LTStub_ILTCursorInst : public ILTCursorInst
{
public:
    LTRESULT IsValid() override { LTSTUB("ILTCursorInst::IsValid"); return LT_ERROR; }
    void SetData(const void *pData) override { LTSTUB("ILTCursorInst::SetData"); }
    void* GetData() override { LTSTUB("ILTCursorInst::GetData"); return 0; }
};

// LTStub_ILTCursor -- from iltcursor.h
class LTStub_ILTCursor : public ILTCursor
{
public:
    LTRESULT SetCursorMode(CursorMode cMode) override { LTSTUB("ILTCursor::SetCursorMode"); return LT_ERROR; }
    LTRESULT GetCursorMode(CursorMode &cMode) override { LTSTUB("ILTCursor::GetCursorMode"); return LT_ERROR; }
    LTRESULT IsCursorModeAvailable(CursorMode cMode) override { LTSTUB("ILTCursor::IsCursorModeAvailable"); return LT_ERROR; }
    LTRESULT LoadCursorBitmapResource(const char *pName, HLTCURSOR &hCursor) override { LTSTUB("ILTCursor::LoadCursorBitmapResource"); return LT_ERROR; }
    LTRESULT FreeCursor(const HLTCURSOR hCursor) override { LTSTUB("ILTCursor::FreeCursor"); return LT_ERROR; }
    LTRESULT SetCursor(HLTCURSOR hCursor) override { LTSTUB("ILTCursor::SetCursor"); return LT_ERROR; }
    LTRESULT IsValidCursor(HLTCURSOR hCursor) override { LTSTUB("ILTCursor::IsValidCursor"); return LT_ERROR; }
    LTRESULT RefreshCursor() override { LTSTUB("ILTCursor::RefreshCursor"); return LT_ERROR; }
};

// LTStub_ILTCustomDraw -- from iltcustomdraw.h
class LTStub_ILTCustomDraw : public ILTCustomDraw
{
public:
    LTRESULT DrawPrimitive(LTVertex *pVerts, uint32 nVerts, uint32 flags) override { LTSTUB("ILTCustomDraw::DrawPrimitive"); return LT_ERROR; }
    LTRESULT SetState(LTRState state, uint32 val) override { LTSTUB("ILTCustomDraw::SetState"); return LT_ERROR; }
    LTRESULT GetState(LTRState state, uint32 &val) override { LTSTUB("ILTCustomDraw::GetState"); return LT_ERROR; }
    LTRESULT SetTexture(const char *pTexture) override { LTSTUB("ILTCustomDraw::SetTexture"); return LT_ERROR; }
    LTRESULT GetTexelSize(float &fSizeU, float &fSizeV) override { LTSTUB("ILTCustomDraw::GetTexelSize"); return LT_ERROR; }
};

// LTStub_ILTDirectMusicMgr -- from iltdirectmusic.h
class LTStub_ILTDirectMusicMgr : public ILTDirectMusicMgr
{
public:
    LTRESULT Init() override { LTSTUB("ILTDirectMusicMgr::Init"); return LT_ERROR; }
    LTRESULT Term() override { LTSTUB("ILTDirectMusicMgr::Term"); return LT_ERROR; }
    LTRESULT InitLevel(const char* sWorkingDirectory, const char* sControlFileName, const char* sDefine1 = NULL, const char* sDefine2 = NULL, const char* sDefine3 = NULL) override { LTSTUB("ILTDirectMusicMgr::InitLevel"); return LT_ERROR; }
    LTRESULT TermLevel() override { LTSTUB("ILTDirectMusicMgr::TermLevel"); return LT_ERROR; }
    LTRESULT Play() override { LTSTUB("ILTDirectMusicMgr::Play"); return LT_ERROR; }
    LTRESULT Stop(const LTDMEnactTypes nStart = LTDMEnactDefault) override { LTSTUB("ILTDirectMusicMgr::Stop"); return LT_ERROR; }
    LTRESULT Pause(const LTDMEnactTypes nStart = LTDMEnactDefault) override { LTSTUB("ILTDirectMusicMgr::Pause"); return LT_ERROR; }
    LTRESULT UnPause() override { LTSTUB("ILTDirectMusicMgr::UnPause"); return LT_ERROR; }
    LTRESULT SetVolume(const long nVolume) override { LTSTUB("ILTDirectMusicMgr::SetVolume"); return LT_ERROR; }
    LTRESULT ChangeIntensity(const int nNewIntensity, const LTDMEnactTypes nStart = LTDMEnactInvalid) override { LTSTUB("ILTDirectMusicMgr::ChangeIntensity"); return LT_ERROR; }
    LTRESULT PlaySecondary(const char* sSecondarySegment, const LTDMEnactTypes nStart = LTDMEnactDefault) override { LTSTUB("ILTDirectMusicMgr::PlaySecondary"); return LT_ERROR; }
    LTRESULT StopSecondary(const char* sSecondarySegment = NULL, const LTDMEnactTypes nStart = LTDMEnactDefault) override { LTSTUB("ILTDirectMusicMgr::StopSecondary"); return LT_ERROR; }
    LTRESULT PlayMotif(const char* sStyleName, const char* sMotifName, const LTDMEnactTypes nStart = LTDMEnactDefault) override { LTSTUB("ILTDirectMusicMgr::PlayMotif"); return LT_ERROR; }
    LTRESULT StopMotif(const char* sStyleName, const char* sMotifName = NULL, const LTDMEnactTypes nStart = LTDMEnactDefault) override { LTSTUB("ILTDirectMusicMgr::StopMotif"); return LT_ERROR; }
    int GetCurIntensity() override { LTSTUB("ILTDirectMusicMgr::GetCurIntensity"); return (int)0; }
    LTDMEnactTypes StringToEnactType(const char* sName) override { LTSTUB("ILTDirectMusicMgr::StringToEnactType"); return (LTDMEnactTypes)0; }
    void EnactTypeToString(LTDMEnactTypes nType, char* sName) override { LTSTUB("ILTDirectMusicMgr::EnactTypeToString"); }
    int GetNumIntensities() override { LTSTUB("ILTDirectMusicMgr::GetNumIntensities"); return (int)0; }
    int GetInitialIntensity() override { LTSTUB("ILTDirectMusicMgr::GetInitialIntensity"); return (int)0; }
    long GetInitialVolume() override { LTSTUB("ILTDirectMusicMgr::GetInitialVolume"); return (long)0; }
};

// LTStub_ILTLightAnim -- from iltlightanim.h
class LTStub_ILTLightAnim : public ILTLightAnim
{
public:
    LTRESULT FindLightAnim(const char *pName, HLIGHTANIM &hLightAnim) override { LTSTUB("ILTLightAnim::FindLightAnim"); return LT_ERROR; }
    LTRESULT GetNumFrames(HLIGHTANIM hLightAnim, uint32 &nFrames) override { LTSTUB("ILTLightAnim::GetNumFrames"); return LT_ERROR; }
    LTRESULT GetLightAnimInfo(HLIGHTANIM hLightAnim, LAInfo &info) override { LTSTUB("ILTLightAnim::GetLightAnimInfo"); return LT_ERROR; }
    LTRESULT SetLightAnimInfo(HLIGHTANIM hLightAnim, LAInfo &info) override { LTSTUB("ILTLightAnim::SetLightAnimInfo"); return LT_ERROR; }
};

// LTStub_ILTPreLight -- from iltprelight.h
class LTStub_ILTPreLight : public ILTPreLight
{
public:
    LTRESULT CreateLightAnim(const char *pName, PreLightAnimFrameInfo *pFrames, uint32 nFrames, LTBOOL bShadowMap) override { LTSTUB("ILTPreLight::CreateLightAnim"); return LT_ERROR; }
    LTRESULT FindObject(const char *pName, HPREOBJECT &hObject) override { LTSTUB("ILTPreLight::FindObject"); return LT_ERROR; }
    LTRESULT GetClassName(HPREOBJECT hObject, char *pClassName, uint32 maxCount) override { LTSTUB("ILTPreLight::GetClassName"); return LT_ERROR; }
    LTRESULT GetPropGeneric(HPREOBJECT hObject, const char *pName, GenericProp *pProp) override { LTSTUB("ILTPreLight::GetPropGeneric"); return LT_ERROR; }
    LTRESULT Parse(ConParse *pParse) override { LTSTUB("ILTPreLight::Parse"); return LT_ERROR; }
    LTRESULT CPrint(const char *pMsg, ...) override { LTSTUB("ILTPreLight::CPrint"); return LT_ERROR; }
    LTRESULT GetWorldModelRuntimePos(HPREOBJECT hObj, LTVector &vPos) override { LTSTUB("ILTPreLight::GetWorldModelRuntimePos"); return LT_ERROR; }

    // Sub-interface DATA members. ⚠️ Not methods and not
    // function pointers -- reached through INLINE accessors, so
    // nothing here ever passes through a stub. Left NULL they are
    // a null read with no [LTSTUB] line before it.
    //
    // Call this from the most-derived constructor, BEFORE any
    // deliberate per-member override: the last assignment wins, and
    // the host has reasons to prefer some variants (the CLIENT
    // flavours of ILTPhysics and ILTSoundMgr, for instance).
    //
    // ⚠️ DECLARED here, DEFINED at the bottom of this header. The
    // stub types it assigns are declared LATER in the file, and an
    // inline body would not see them: a member function body is
    // parsed in its own class's complete-class context, which
    // delays lookup for THAT class's members only, not for types
    // that appear further down.
    void LTSkel_WireData();
};

// LTStub_ILTSpriteControl -- from iltspritecontrol.h
class LTStub_ILTSpriteControl : public ILTSpriteControl
{
public:
    LTRESULT GetNumAnims(uint32 &nAnims) override { LTSTUB("ILTSpriteControl::GetNumAnims"); return LT_ERROR; }
    LTRESULT GetNumFrames(uint32 iAnim, uint32 &nFrames) override { LTSTUB("ILTSpriteControl::GetNumFrames"); return LT_ERROR; }
    LTRESULT GetCurPos(uint32 &iAnim, uint32 &iFrame) override { LTSTUB("ILTSpriteControl::GetCurPos"); return LT_ERROR; }
    LTRESULT SetCurPos(uint32 iAnim, uint32 iFrame) override { LTSTUB("ILTSpriteControl::SetCurPos"); return LT_ERROR; }
    LTRESULT GetFlags(uint32 &flags) override { LTSTUB("ILTSpriteControl::GetFlags"); return LT_ERROR; }
    LTRESULT SetFlags(uint32 flags) override { LTSTUB("ILTSpriteControl::SetFlags"); return LT_ERROR; }
};

// LTStub_ILTTexMod -- from ilttexmod.h
class LTStub_ILTTexMod : public ILTTexMod
{
public:
    LTRESULT GetTextureHandle(char *pFilename, HTEXTURE &hTexture, const uint32 flags) override { LTSTUB("ILTTexMod::GetTextureHandle"); return LT_ERROR; }
    LTRESULT ReleaseTextureHandle(const HTEXTURE hTexture) override { LTSTUB("ILTTexMod::ReleaseTextureHandle"); return LT_ERROR; }
    LTRESULT GetTextureInfo(const HTEXTURE hTexture, TextureInfo &info) override { LTSTUB("ILTTexMod::GetTextureInfo"); return LT_ERROR; }
    LTRESULT WasTextureDrawnLastFrame(const HTEXTURE hTexture) override { LTSTUB("ILTTexMod::WasTextureDrawnLastFrame"); return LT_ERROR; }
    LTRESULT LockTexture(const HTEXTURE hTexture, const LTRect *pRect, const uint32 lockType, uint8* &pData, long &lPitch) override { LTSTUB("ILTTexMod::LockTexture"); return LT_ERROR; }
    LTRESULT UnlockTexture(const HTEXTURE hTexture) override { LTSTUB("ILTTexMod::UnlockTexture"); return LT_ERROR; }
};

// LTStub_ILTVideoMgr -- from iltvideomgr.h
class LTStub_ILTVideoMgr : public ILTVideoMgr
{
public:
    LTRESULT StartOnScreenVideo(char *pFilename, uint32 flags, HVIDEO &hVideo) override { LTSTUB("ILTVideoMgr::StartOnScreenVideo"); return LT_ERROR; }
    LTRESULT StartTextureVideo(char *pFilename, uint32 flags, HVIDEO &hVideo) override { LTSTUB("ILTVideoMgr::StartTextureVideo"); return LT_ERROR; }
    LTRESULT UpdateVideo(HVIDEO hVideo) override { LTSTUB("ILTVideoMgr::UpdateVideo"); return LT_ERROR; }
    LTRESULT GetVideoStatus(HVIDEO hVideo) override { LTSTUB("ILTVideoMgr::GetVideoStatus"); return LT_ERROR; }
    LTRESULT StopVideo(HVIDEO hVideo) override { LTSTUB("ILTVideoMgr::StopVideo"); return LT_ERROR; }
    LTRESULT BindTextureVideoToPoly(HVIDEO hVideo, HPOLY hPoly) override { LTSTUB("ILTVideoMgr::BindTextureVideoToPoly"); return LT_ERROR; }
};

#endif // __LT_ENGINE_SKELETON_GENERATED_H__

// --- sub-interface data-member wiring ------------------------------
// Defined here, not inline in the classes: the stub types assigned below
// are declared further up but AFTER the classes that use them.
inline void LTStub_ILTCSBase::LTSkel_WireData()
{
    { static LTStub_ILTCommon s; m_pCommonLT = &s; }
    { static LTStub_ILTPhysics s; m_pPhysicsLT = &s; }
    { static LTStub_ILTModel s; m_pModelLT = &s; }
    { static LTStub_ILTTransform s; m_pTransformLT = &s; }
    { static LTStub_ILTLightAnim s; m_pLightAnimLT = &s; }
    { static LTStub_ILTSoundMgr s; m_pSoundMgr = &s; }
}
inline void LTStub_ILTClient::LTSkel_WireData()
{
    { static LTStub_ILTVideoMgr s; m_pVideoMgr = &s; }
    { static LTStub_ILTTexMod s; m_pTexMod = &s; }
    { static LTStub_ILTCursor s; m_pCursorLT = &s; }
    { static LTStub_ILTDirectMusicMgr s; m_pDirectMusicMgr = &s; }
    { static LTStub_ILTCommon s; m_pCommonLT = &s; }
    { static LTStub_ILTPhysics s; m_pPhysicsLT = &s; }
    { static LTStub_ILTModel s; m_pModelLT = &s; }
    { static LTStub_ILTTransform s; m_pTransformLT = &s; }
    { static LTStub_ILTLightAnim s; m_pLightAnimLT = &s; }
    { static LTStub_ILTSoundMgr s; m_pSoundMgr = &s; }
}
inline void LTStub_ILTServer::LTSkel_WireData()
{
    { static LTStub_ILTCommon s; m_pCommonLT = &s; }
    { static LTStub_ILTPhysics s; m_pPhysicsLT = &s; }
    { static LTStub_ILTModel s; m_pModelLT = &s; }
    { static LTStub_ILTTransform s; m_pTransformLT = &s; }
    { static LTStub_ILTLightAnim s; m_pLightAnimLT = &s; }
    { static LTStub_ILTSoundMgr s; m_pSoundMgr = &s; }
}
inline void LTStub_ILTCommon::LTSkel_WireData()
{
    { static LTStub_ILTTransform s; m_pTransformLT = &s; }
    { static LTStub_ILTModel s; m_pModelLT = &s; }
}
inline void LTStub_ILTPreLight::LTSkel_WireData()
{
    { static LTStub_ILTMath s; m_pMathLT = &s; }
}

// --- function-pointer members: stubs -------------------------------
static LTRESULT LTFnStub_ILTClient_StartGame(StartGameRequest *pRequest) { LTSTUB("ILTClient::StartGame"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetGameMode(int *mode) { LTSTUB("ILTClient::GetGameMode"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetLocalClientID(uint32 *pID) { LTSTUB("ILTClient::GetLocalClientID"); return LT_ERROR; }
static LTBOOL LTFnStub_ILTClient_IsConnected() { LTSTUB("ILTClient::IsConnected"); return LTFALSE; }
static void LTFnStub_ILTClient_Disconnect() { LTSTUB("ILTClient::Disconnect"); }
static void LTFnStub_ILTClient_Shutdown() { LTSTUB("ILTClient::Shutdown"); }
static void LTFnStub_ILTClient_ShutdownWithMessage(char *pMsg, ...) { LTSTUB("ILTClient::ShutdownWithMessage"); }
static LTRESULT LTFnStub_ILTClient_FlipScreen(uint32 flags) { LTSTUB("ILTClient::FlipScreen"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_ClearScreen(LTRect *pClearRect, uint32 flags) { LTSTUB("ILTClient::ClearScreen"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_Start3D() { LTSTUB("ILTClient::Start3D"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_RenderCamera(HLOCALOBJ hCamera) { LTSTUB("ILTClient::RenderCamera"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_RenderObjects(HLOCALOBJ hCamera, HLOCALOBJ *pObjects, int nObjects) { LTSTUB("ILTClient::RenderObjects"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_StartOptimized2D() { LTSTUB("ILTClient::StartOptimized2D"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_EndOptimized2D() { LTSTUB("ILTClient::EndOptimized2D"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_SetOptimized2DBlend(LTSurfaceBlend blend) { LTSTUB("ILTClient::SetOptimized2DBlend"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetOptimized2DBlend(LTSurfaceBlend &blend) { LTSTUB("ILTClient::GetOptimized2DBlend"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_SetOptimized2DColor(HLTCOLOR hColor) { LTSTUB("ILTClient::SetOptimized2DColor"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetOptimized2DColor(HLTCOLOR &hColor) { LTSTUB("ILTClient::GetOptimized2DColor"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_End3D() { LTSTUB("ILTClient::End3D"); return LT_ERROR; }
static RMode* LTFnStub_ILTClient_GetRenderModes() { LTSTUB("ILTClient::GetRenderModes"); return 0; }
static void LTFnStub_ILTClient_RelinquishRenderModes(RMode *pModes) { LTSTUB("ILTClient::RelinquishRenderModes"); }
static LTRESULT LTFnStub_ILTClient_GetRenderMode(RMode *pMode) { LTSTUB("ILTClient::GetRenderMode"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_SetRenderMode(RMode *pMode) { LTSTUB("ILTClient::SetRenderMode"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_ShutdownRender(uint32 flags) { LTSTUB("ILTClient::ShutdownRender"); return LT_ERROR; }
static FileEntry* LTFnStub_ILTClient_GetFileList(char *pDirName) { LTSTUB("ILTClient::GetFileList"); return 0; }
static void LTFnStub_ILTClient_FreeFileList(FileEntry *pHead) { LTSTUB("ILTClient::FreeFileList"); }
static LTRESULT LTFnStub_ILTClient_GetWorldInfoString(char *pFilename, char *pInfoString, uint32 maxLen, uint32 *pActualLen) { LTSTUB("ILTClient::GetWorldInfoString"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_ReadConfigFile(char *pFilename) { LTSTUB("ILTClient::ReadConfigFile"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_WriteConfigFile(char *pFilename) { LTSTUB("ILTClient::WriteConfigFile"); return LT_ERROR; }
static void LTFnStub_ILTClient_GetAxisOffsets(LTFLOAT *offsets) { LTSTUB("ILTClient::GetAxisOffsets"); }
static void LTFnStub_ILTClient_PlayJoystickEffect(char *pEffectName, float x, float y) { LTSTUB("ILTClient::PlayJoystickEffect"); }
static LTBOOL LTFnStub_ILTClient_InitMusic(char *szMusicDLL) { LTSTUB("ILTClient::InitMusic"); return LTFALSE; }
static LTBOOL LTFnStub_ILTClient_SetMusicDirectory(char *szMusicDirectory) { LTSTUB("ILTClient::SetMusicDirectory"); return LTFALSE; }
static LTBOOL LTFnStub_ILTClient_InitInstruments(char *szDLSFile, char *szStyleFile) { LTSTUB("ILTClient::InitInstruments"); return LTFALSE; }
static LTBOOL LTFnStub_ILTClient_LoadSong(char *szSong) { LTSTUB("ILTClient::LoadSong"); return LTFALSE; }
static void LTFnStub_ILTClient_DestroyAllSongs() { LTSTUB("ILTClient::DestroyAllSongs"); }
static LTBOOL LTFnStub_ILTClient_PlayList(char *szPlayList, char *szTransition, LTBOOL bLoop, uint32 dwBoundaryFlags) { LTSTUB("ILTClient::PlayList"); return LTFALSE; }
static LTBOOL LTFnStub_ILTClient_PlayBreak(char *szSong, uint32 dwBoundaryFlags) { LTSTUB("ILTClient::PlayBreak"); return LTFALSE; }
static void LTFnStub_ILTClient_StopMusic(uint32 dwBoundaryFlags) { LTSTUB("ILTClient::StopMusic"); }
static LTBOOL LTFnStub_ILTClient_PauseMusic() { LTSTUB("ILTClient::PauseMusic"); return LTFALSE; }
static LTBOOL LTFnStub_ILTClient_ResumeMusic() { LTSTUB("ILTClient::ResumeMusic"); return LTFALSE; }
static LTBOOL LTFnStub_ILTClient_AddSongToPlayList(char *szPlayList, char *szSong) { LTSTUB("ILTClient::AddSongToPlayList"); return LTFALSE; }
static void LTFnStub_ILTClient_DeletePlayList(char *szPlayList) { LTSTUB("ILTClient::DeletePlayList"); }
static LTBOOL LTFnStub_ILTClient_PlayMotif(char *szMotif, LTBOOL bLoop) { LTSTUB("ILTClient::PlayMotif"); return LTFALSE; }
static void LTFnStub_ILTClient_StopMotif(char *szMotif) { LTSTUB("ILTClient::StopMotif"); }
static short LTFnStub_ILTClient_GetMusicVolume() { LTSTUB("ILTClient::GetMusicVolume"); return (short)0; }
static void LTFnStub_ILTClient_SetMusicVolume(short wVolume) { LTSTUB("ILTClient::SetMusicVolume"); }
static LTRESULT LTFnStub_ILTClient_GetSound3DProviderLists(Sound3DProvider *&pSound3DProviderList, LTBOOL bVerify) { LTSTUB("ILTClient::GetSound3DProviderLists"); return LT_ERROR; }
static void LTFnStub_ILTClient_ReleaseSound3DProviderList(Sound3DProvider *pSound3DProviderList) { LTSTUB("ILTClient::ReleaseSound3DProviderList"); }
static LTRESULT LTFnStub_ILTClient_InitSound(InitSoundInfo *pSoundInfo) { LTSTUB("ILTClient::InitSound"); return LT_ERROR; }
static unsigned short LTFnStub_ILTClient_GetSoundVolume() { LTSTUB("ILTClient::GetSoundVolume"); return (unsigned short)0; }
static void LTFnStub_ILTClient_SetSoundVolume(unsigned short nVolume) { LTSTUB("ILTClient::SetSoundVolume"); }
static LTRESULT LTFnStub_ILTClient_SetReverbProperties(ReverbProperties *pReverbProperties) { LTSTUB("ILTClient::SetReverbProperties"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetReverbProperties(ReverbProperties *pReverbProperties) { LTSTUB("ILTClient::GetReverbProperties"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_PlaySound(PlaySoundInfo *pPlaySoundInfo) { LTSTUB("ILTClient::PlaySound"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_SetSoundPosition(HLTSOUND hSound, LTVector *pPos) { LTSTUB("ILTClient::SetSoundPosition"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetSoundPosition(HLTSOUND hSound, LTVector *pPos) { LTSTUB("ILTClient::GetSoundPosition"); return LT_ERROR; }
static void LTFnStub_ILTClient_PauseSounds() { LTSTUB("ILTClient::PauseSounds"); }
static void LTFnStub_ILTClient_ResumeSounds() { LTSTUB("ILTClient::ResumeSounds"); }
static LTRESULT LTFnStub_ILTClient_GetSoundDuration(HLTSOUND hSound, LTFLOAT *fDuration) { LTSTUB("ILTClient::GetSoundDuration"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetSoundTimer(HLTSOUND hSound, LTFLOAT *fTimer) { LTSTUB("ILTClient::GetSoundTimer"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetSoundData(HLTSOUND hSound, int16 * & pSixteenBitData, int8 * & pEightBitData, uint32 * dwSamplesPerSecond, uint32 * dwChannels) { LTSTUB("ILTClient::GetSoundData"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetSoundOffset(HLTSOUND hSound, uint32 * dwOffset, uint32 * dwSize) { LTSTUB("ILTClient::GetSoundOffset"); return LT_ERROR; }
static LTBOOL LTFnStub_ILTClient_IsDone(HLTSOUND pSoundHandle) { LTSTUB("ILTClient::IsDone"); return LTFALSE; }
static void LTFnStub_ILTClient_KillSound(HLTSOUND pSoundHandle) { LTSTUB("ILTClient::KillSound"); }
static void LTFnStub_ILTClient_KillSoundLoop(HLTSOUND hSoundHandle) { LTSTUB("ILTClient::KillSoundLoop"); }
static void LTFnStub_ILTClient_SetListener(LTBOOL bListenerInClient, LTVector *pPos, LTRotation *pRot) { LTSTUB("ILTClient::SetListener"); }
static void LTFnStub_ILTClient_GetListener(LTBOOL *bListenerInClient, LTVector *pPos, LTRotation *pRot) { LTSTUB("ILTClient::GetListener"); }
static LTBOOL LTFnStub_ILTClient_IntersectSegment(ClientIntersectQuery *pQuery, ClientIntersectInfo *pInfo) { LTSTUB("ILTClient::IntersectSegment"); return LTFALSE; }
static LTBOOL LTFnStub_ILTClient_CastRay(ClientIntersectQuery *pQuery, ClientIntersectInfo *pInfo) { LTSTUB("ILTClient::CastRay"); return LTFALSE; }
static LTRESULT LTFnStub_ILTClient_FindObjectsInSphere(LTVector *pCenter, float radius, HLOCALOBJ *inObjects, uint32 nInObjects, uint32 *nOutObjects, uint32 *nFound) { LTSTUB("ILTClient::FindObjectsInSphere"); return LT_ERROR; }
static HLTFONT LTFnStub_ILTClient_CreateFont(char *pFontName, int width, int height, LTBOOL bItalic, LTBOOL bUnderline, LTBOOL bBold) { LTSTUB("ILTClient::CreateFont"); return (HLTFONT)0; }
static void LTFnStub_ILTClient_DeleteFont(HLTFONT hFont) { LTSTUB("ILTClient::DeleteFont"); }
static LTRESULT LTFnStub_ILTClient_SetFontExtraSpace(HLTFONT hFont, int pixels) { LTSTUB("ILTClient::SetFontExtraSpace"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetFontExtraSpace(HLTFONT hFont, int &pixels) { LTSTUB("ILTClient::GetFontExtraSpace"); return LT_ERROR; }
static HLTCOLOR LTFnStub_ILTClient_CreateColor(float r, float g, float b, LTBOOL bTransparent) { LTSTUB("ILTClient::CreateColor"); return (HLTCOLOR)0; }
static void LTFnStub_ILTClient_DeleteColor(HLTCOLOR hColor) { LTSTUB("ILTClient::DeleteColor"); }
static HLTCOLOR LTFnStub_ILTClient_SetupColor1(float r, float g, float b, LTBOOL bTransparent) { LTSTUB("ILTClient::SetupColor1"); return (HLTCOLOR)0; }
static HLTCOLOR LTFnStub_ILTClient_SetupColor2(float r, float g, float b, LTBOOL bTransparent) { LTSTUB("ILTClient::SetupColor2"); return (HLTCOLOR)0; }
static LTRESULT LTFnStub_ILTClient_GetBorderSize(HSURFACE hSurface, HLTCOLOR hColor, LTRect *pRect) { LTSTUB("ILTClient::GetBorderSize"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_OptimizeSurface(HSURFACE hSurface, HLTCOLOR hTransparentColor) { LTSTUB("ILTClient::OptimizeSurface"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_UnoptimizeSurface(HSURFACE hSurface) { LTSTUB("ILTClient::UnoptimizeSurface"); return LT_ERROR; }
static HSURFACE LTFnStub_ILTClient_GetScreenSurface() { LTSTUB("ILTClient::GetScreenSurface"); return (HSURFACE)0; }
static HSURFACE LTFnStub_ILTClient_CreateSurfaceFromBitmap(char *pBitmapName) { LTSTUB("ILTClient::CreateSurfaceFromBitmap"); return (HSURFACE)0; }
static HSURFACE LTFnStub_ILTClient_CreateSurfaceFromString(HLTFONT hFont, HSTRING hString, HLTCOLOR hForeColor, HLTCOLOR hBackColor, int extraPixelsX, int extraPixelsY) { LTSTUB("ILTClient::CreateSurfaceFromString"); return (HSURFACE)0; }
static HSURFACE LTFnStub_ILTClient_CreateSurface(uint32 width, uint32 height) { LTSTUB("ILTClient::CreateSurface"); return (HSURFACE)0; }
static LTRESULT LTFnStub_ILTClient_DeleteSurface(HSURFACE hSurface) { LTSTUB("ILTClient::DeleteSurface"); return LT_ERROR; }
static void* LTFnStub_ILTClient_GetSurfaceUserData(HSURFACE hSurface) { LTSTUB("ILTClient::GetSurfaceUserData"); return 0; }
static void LTFnStub_ILTClient_SetSurfaceUserData(HSURFACE hSurface, void *pUserData) { LTSTUB("ILTClient::SetSurfaceUserData"); }
static LTRESULT LTFnStub_ILTClient_GetPixel(HSURFACE hSurface, uint32 x, uint32 y, HLTCOLOR *color) { LTSTUB("ILTClient::GetPixel"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_SetPixel(HSURFACE hSurface, uint32 x, uint32 y, HLTCOLOR color) { LTSTUB("ILTClient::SetPixel"); return LT_ERROR; }
static void LTFnStub_ILTClient_GetStringDimensions(HLTFONT hFont, HSTRING hString, int *sizeX, int *sizeY) { LTSTUB("ILTClient::GetStringDimensions"); }
static void LTFnStub_ILTClient_DrawStringToSurface(HSURFACE hDest, HLTFONT hFont, HSTRING hString, LTRect *pRect, HLTCOLOR hForeColor, HLTCOLOR hBackColor) { LTSTUB("ILTClient::DrawStringToSurface"); }
static void LTFnStub_ILTClient_GetSurfaceDims(HSURFACE hSurf, uint32 *pWidth, uint32 *pHeight) { LTSTUB("ILTClient::GetSurfaceDims"); }
static LTBOOL LTFnStub_ILTClient_DrawBitmapToSurface(HSURFACE hDest, char *pSourceBitmapName, LTRect *pSrcRect, int destX, int destY) { LTSTUB("ILTClient::DrawBitmapToSurface"); return LTFALSE; }
static LTRESULT LTFnStub_ILTClient_DrawSurfaceMasked(HSURFACE hDest, HSURFACE hSrc, HSURFACE hMask, LTRect *pSrcRect, int destX, int destY, HLTCOLOR hColor) { LTSTUB("ILTClient::DrawSurfaceMasked"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_DrawSurfaceSolidColor(HSURFACE hDest, HSURFACE hSrc, LTRect *pSrcRect, int destX, int destY, HLTCOLOR hTransColor, HLTCOLOR hFillColor) { LTSTUB("ILTClient::DrawSurfaceSolidColor"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_DrawSurfaceToSurface(HSURFACE hDest, HSURFACE hSrc, LTRect *pSrcRect, int destX, int destY) { LTSTUB("ILTClient::DrawSurfaceToSurface"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_DrawSurfaceToSurfaceTransparent(HSURFACE hDest, HSURFACE hSrc, LTRect *pSrcRect, int destX, int destY, HLTCOLOR hColor) { LTSTUB("ILTClient::DrawSurfaceToSurfaceTransparent"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_ScaleSurfaceToSurface(HSURFACE hDest, HSURFACE hSrc, LTRect *pDestRect, LTRect *pSrcRect) { LTSTUB("ILTClient::ScaleSurfaceToSurface"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_ScaleSurfaceToSurfaceTransparent(HSURFACE hDest, HSURFACE hSrc, LTRect *pDestRect, LTRect *pSrcRect, HLTCOLOR hColor) { LTSTUB("ILTClient::ScaleSurfaceToSurfaceTransparent"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_ScaleSurfaceToSurfaceSolidColor(HSURFACE hDest, HSURFACE hSrc, LTRect *pDestRect, LTRect *pSrcRect, HLTCOLOR hTransColor, HLTCOLOR hFillColor) { LTSTUB("ILTClient::ScaleSurfaceToSurfaceSolidColor"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_WarpSurfaceToSurface(HSURFACE hDest, HSURFACE hSrc, LTWarpPt *pCoords, int nCoords) { LTSTUB("ILTClient::WarpSurfaceToSurface"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_WarpSurfaceToSurfaceTransparent(HSURFACE hDest, HSURFACE hSrc, LTWarpPt *pCoords, int nCoords, HLTCOLOR hColor) { LTSTUB("ILTClient::WarpSurfaceToSurfaceTransparent"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_WarpSurfaceToSurfaceSolidColor(HSURFACE hDest, HSURFACE hSrc, LTWarpPt *pCoords, int nCoords, HLTCOLOR hTransColor, HLTCOLOR hFillColor) { LTSTUB("ILTClient::WarpSurfaceToSurfaceSolidColor"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_TransformSurfaceToSurface(HSURFACE hDest, HSURFACE hSrc, LTFloatPt *pOrigin, int destX, int destY, float angle, float scaleX, float scaleY) { LTSTUB("ILTClient::TransformSurfaceToSurface"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_TransformSurfaceToSurfaceTransparent(HSURFACE hDest, HSURFACE hSrc, LTFloatPt *pOrigin, int destX, int destY, float angle, float scaleX, float scaleY, HLTCOLOR hColor) { LTSTUB("ILTClient::TransformSurfaceToSurfaceTransparent"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_FillRect(HSURFACE hDest, LTRect *pRect, HLTCOLOR hColor) { LTSTUB("ILTClient::FillRect"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetSurfaceAlpha(HSURFACE hSurface, float &alpha) { LTSTUB("ILTClient::GetSurfaceAlpha"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_SetSurfaceAlpha(HSURFACE hSurface, float alpha) { LTSTUB("ILTClient::SetSurfaceAlpha"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_RegisterConsoleProgram(char *pName, ConsoleProgramFn fn) { LTSTUB("ILTClient::RegisterConsoleProgram"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_UnregisterConsoleProgram(char *pName) { LTSTUB("ILTClient::UnregisterConsoleProgram"); return LT_ERROR; }
static HCONSOLEVAR LTFnStub_ILTClient_GetConsoleVar(char *pName) { LTSTUB("ILTClient::GetConsoleVar"); return (HCONSOLEVAR)0; }
static void LTFnStub_ILTClient_StartCounter(LTCounter *pCounter) { LTSTUB("ILTClient::StartCounter"); }
static uint32 LTFnStub_ILTClient_EndCounter(LTCounter *pCounter) { LTSTUB("ILTClient::EndCounter"); return (uint32)0; }
static LTRESULT LTFnStub_ILTClient_SetGlobalPanTexture(uint32 index, char *pFilename) { LTSTUB("ILTClient::SetGlobalPanTexture"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_SetGlobalPanInfo(uint32 index, float xOffset, float zOffset, float xScale, float zScale) { LTSTUB("ILTClient::SetGlobalPanInfo"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_AddSurfaceEffect(SurfaceEffectDesc *pDesc) { LTSTUB("ILTClient::AddSurfaceEffect"); return LT_ERROR; }
static void LTFnStub_ILTClient_SetInputState(LTBOOL bOn) { LTSTUB("ILTClient::SetInputState"); }
static LTRESULT LTFnStub_ILTClient_ClearInput() { LTSTUB("ILTClient::ClearInput"); return LT_ERROR; }
static DeviceBinding* LTFnStub_ILTClient_GetDeviceBindings(uint32 nDevice) { LTSTUB("ILTClient::GetDeviceBindings"); return 0; }
static void LTFnStub_ILTClient_FreeDeviceBindings(DeviceBinding* pBindings) { LTSTUB("ILTClient::FreeDeviceBindings"); }
static LTRESULT LTFnStub_ILTClient_StartDeviceTrack(uint32 nDevices, uint32 nBufferSize) { LTSTUB("ILTClient::StartDeviceTrack"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_TrackDevice(DeviceInput* pInputArray, uint32* pnInOut) { LTSTUB("ILTClient::TrackDevice"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_EndDeviceTrack() { LTSTUB("ILTClient::EndDeviceTrack"); return LT_ERROR; }
static DeviceObject* LTFnStub_ILTClient_GetDeviceObjects(uint32 nDeviceFlags) { LTSTUB("ILTClient::GetDeviceObjects"); return 0; }
static void LTFnStub_ILTClient_FreeDeviceObjects(DeviceObject* pList) { LTSTUB("ILTClient::FreeDeviceObjects"); }
static LTRESULT LTFnStub_ILTClient_GetDeviceName(uint32 nDeviceType, char* pStrBuffer, uint32 nBufferSize) { LTSTUB("ILTClient::GetDeviceName"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_IsDeviceEnabled(char* strDeviceName, LTBOOL* pIsEnabled) { LTSTUB("ILTClient::IsDeviceEnabled"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_EnableDevice(char* strDeviceName) { LTSTUB("ILTClient::EnableDevice"); return LT_ERROR; }
static float LTFnStub_ILTClient_GetGameTime() { LTSTUB("ILTClient::GetGameTime"); return 0.0f; }
static float LTFnStub_ILTClient_GetGameFrameTime() { LTSTUB("ILTClient::GetGameFrameTime"); return 0.0f; }
static void LTFnStub_ILTClient_DebugOut(char *pMsg, ...) { LTSTUB("ILTClient::DebugOut"); }
static LTRESULT LTFnStub_ILTClient_GetSkyDef(SkyDef *pDef) { LTSTUB("ILTClient::GetSkyDef"); return LT_ERROR; }
static LTBOOL LTFnStub_ILTClient_IsCommandOn(int commandNum) { LTSTUB("ILTClient::IsCommandOn"); return LTFALSE; }
static void LTFnStub_ILTClient_RunConsoleString(char *pString) { LTSTUB("ILTClient::RunConsoleString"); }
static HLOCALOBJ LTFnStub_ILTClient_GetClientObject() { LTSTUB("ILTClient::GetClientObject"); return (HLOCALOBJ)0; }
static void LTFnStub_ILTClient_GetGlobalLightScale(LTVector *pScale) { LTSTUB("ILTClient::GetGlobalLightScale"); }
static void LTFnStub_ILTClient_SetGlobalLightScale(LTVector *pScale) { LTSTUB("ILTClient::SetGlobalLightScale"); }
static void LTFnStub_ILTClient_OffsetGlobalLightScale(LTVector *pOffset) { LTSTUB("ILTClient::OffsetGlobalLightScale"); }
static int LTFnStub_ILTClient_Parse(char *pCommand, char **pNewCommandPos, char *argBuffer, char **argPointers, int *nArgs) { LTSTUB("ILTClient::Parse"); return (int)0; }
static HLOCALOBJ LTFnStub_ILTClient_CreateObject(ObjectCreateStruct *pStruct) { LTSTUB("ILTClient::CreateObject"); return (HLOCALOBJ)0; }
static void LTFnStub_ILTClient_GetObjectPos(HLOCALOBJ hObj, LTVector *pPos) { LTSTUB("ILTClient::GetObjectPos"); }
static void LTFnStub_ILTClient_GetObjectRotation(HLOCALOBJ hObj, LTRotation *pRotation) { LTSTUB("ILTClient::GetObjectRotation"); }
static void LTFnStub_ILTClient_SetObjectRotation(HLOCALOBJ hObj, LTRotation *pRotation) { LTSTUB("ILTClient::SetObjectRotation"); }
static void LTFnStub_ILTClient_SetObjectPosAndRotation(HLOCALOBJ hObj, LTVector *pPos, LTRotation *pRotation) { LTSTUB("ILTClient::SetObjectPosAndRotation"); }
static LTRESULT LTFnStub_ILTClient_GetObjectScale(HLOCALOBJ hObj, LTVector *pScale) { LTSTUB("ILTClient::GetObjectScale"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_SetObjectScale(HLOCALOBJ hObj, LTVector *pScale) { LTSTUB("ILTClient::SetObjectScale"); return LT_ERROR; }
static void LTFnStub_ILTClient_GetObjectColor(HLOCALOBJ hObject, float *r, float *g, float *b, float *a) { LTSTUB("ILTClient::GetObjectColor"); }
static void LTFnStub_ILTClient_SetObjectColor(HLOCALOBJ hObject, float r, float g, float b, float a) { LTSTUB("ILTClient::SetObjectColor"); }
static LTRESULT LTFnStub_ILTClient_GetObjectUserFlags(HLOCALOBJ hObj, uint32 *pFlags) { LTSTUB("ILTClient::GetObjectUserFlags"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_SetObjectUserFlags(HLOCALOBJ hObj, uint32 flags) { LTSTUB("ILTClient::SetObjectUserFlags"); return LT_ERROR; }
static uint32 LTFnStub_ILTClient_GetObjectClientFlags(HLOCALOBJ hObj) { LTSTUB("ILTClient::GetObjectClientFlags"); return (uint32)0; }
static void LTFnStub_ILTClient_SetObjectClientFlags(HLOCALOBJ hObj, uint32 flags) { LTSTUB("ILTClient::SetObjectClientFlags"); }
static void* LTFnStub_ILTClient_GetObjectUserData(HLOCALOBJ hObj) { LTSTUB("ILTClient::GetObjectUserData"); return 0; }
static void LTFnStub_ILTClient_SetObjectUserData(HLOCALOBJ hObj, void *pData) { LTSTUB("ILTClient::SetObjectUserData"); }
static LTRESULT LTFnStub_ILTClient_Get3DCameraPt(HLOCALOBJ hCamera, int sx, int sy, LTVector *pOut) { LTSTUB("ILTClient::Get3DCameraPt"); return LT_ERROR; }
static void LTFnStub_ILTClient_GetCameraFOV(HLOCALOBJ hObj, float *pX, float *pY) { LTSTUB("ILTClient::GetCameraFOV"); }
static void LTFnStub_ILTClient_SetCameraFOV(HLOCALOBJ hObj, float fovX, float fovY) { LTSTUB("ILTClient::SetCameraFOV"); }
static void LTFnStub_ILTClient_GetCameraRect(HLOCALOBJ hObj, LTBOOL *bFullscreen, int *left, int *top, int *right, int *bottom) { LTSTUB("ILTClient::GetCameraRect"); }
static void LTFnStub_ILTClient_SetCameraRect(HLOCALOBJ hObj, LTBOOL bFullscreen, int left, int top, int right, int bottom) { LTSTUB("ILTClient::SetCameraRect"); }
static LTBOOL LTFnStub_ILTClient_GetCameraLightAdd(HLOCALOBJ hCamera, LTVector *pAdd) { LTSTUB("ILTClient::GetCameraLightAdd"); return LTFALSE; }
static LTBOOL LTFnStub_ILTClient_SetCameraLightAdd(HLOCALOBJ hCamera, LTVector *pAdd) { LTSTUB("ILTClient::SetCameraLightAdd"); return LTFALSE; }
static LTRESULT LTFnStub_ILTClient_SetupParticleSystem(HLOCALOBJ hObj, char *pTextureName, float gravityAccel, uint32 flags, float particleRadius) { LTSTUB("ILTClient::SetupParticleSystem"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_SetSoftwarePSColor(HLOCALOBJ hObj, float r, float g, float b) { LTSTUB("ILTClient::SetSoftwarePSColor"); return LT_ERROR; }
static void LTFnStub_ILTClient_AddParticles(HLOCALOBJ hObj, uint32 nParticles, LTVector *pMinOffset, LTVector *pMaxOffset, LTVector *pMinVelocity, LTVector *pMaxVelocity, LTVector *pMinColor, LTVector *pMaxColor, float minLifetime, float maxLifetime) { LTSTUB("ILTClient::AddParticles"); }
static LTBOOL LTFnStub_ILTClient_GetParticles(HLOCALOBJ hObj, LTParticle **pHead, LTParticle **pTail) { LTSTUB("ILTClient::GetParticles"); return LTFALSE; }
static void LTFnStub_ILTClient_GetParticlePos(HLOCALOBJ hSystem, LTParticle *pParticle, LTVector *pPos) { LTSTUB("ILTClient::GetParticlePos"); }
static void LTFnStub_ILTClient_SetParticlePos(HLOCALOBJ hSystem, LTParticle *pParticle, LTVector *pPos) { LTSTUB("ILTClient::SetParticlePos"); }
static LTRESULT LTFnStub_ILTClient_GetParticleLifetime(HLOCALOBJ hSystem, LTParticle *pParticle, LTFLOAT & fLifetime) { LTSTUB("ILTClient::GetParticleLifetime"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetParticleTotalLifetime(HLOCALOBJ hSystem, LTParticle *pParticle, LTFLOAT & fTotalLifetime) { LTSTUB("ILTClient::GetParticleTotalLifetime"); return LT_ERROR; }
static void LTFnStub_ILTClient_RemoveParticle(HLOCALOBJ hSystem, LTParticle *pParticle) { LTSTUB("ILTClient::RemoveParticle"); }
static LTRESULT LTFnStub_ILTClient_OptimizeParticles(HLOCALOBJ hSystem) { LTSTUB("ILTClient::OptimizeParticles"); return LT_ERROR; }
static HLTLINE LTFnStub_ILTClient_GetNextLine(HLOCALOBJ hObj, HLTLINE hPrev) { LTSTUB("ILTClient::GetNextLine"); return (HLTLINE)0; }
static void LTFnStub_ILTClient_GetLineInfo(HLTLINE hLine, LTLine *pLine) { LTSTUB("ILTClient::GetLineInfo"); }
static void LTFnStub_ILTClient_SetLineInfo(HLTLINE hLine, LTLine *pLine) { LTSTUB("ILTClient::SetLineInfo"); }
static HLTLINE LTFnStub_ILTClient_AddLine(HLOCALOBJ hObj, LTLine *pLine) { LTSTUB("ILTClient::AddLine"); return (HLTLINE)0; }
static void LTFnStub_ILTClient_RemoveLine(HLOCALOBJ hObj, HLTLINE hLine) { LTSTUB("ILTClient::RemoveLine"); }
static LTBOOL LTFnStub_ILTClient_SetupPolyGrid(HLOCALOBJ hObj, uint32 width, uint32 height, LTBOOL bHalfTrianges) { LTSTUB("ILTClient::SetupPolyGrid"); return LTFALSE; }
static LTRESULT LTFnStub_ILTClient_SetPolyGridTexture(HLOCALOBJ hObj, char *pFilename) { LTSTUB("ILTClient::SetPolyGridTexture"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_SetPolyGridEnvMap(HLOCALOBJ hObj, char *pFilename) { LTSTUB("ILTClient::SetPolyGridEnvMap"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetPolyGridTextureInfo(HLOCALOBJ hObj, float *xPan, float *yPan, float *xScale, float *yScale) { LTSTUB("ILTClient::GetPolyGridTextureInfo"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_SetPolyGridTextureInfo(HLOCALOBJ hObj, float xPan, float yPan, float xScale, float yScale) { LTSTUB("ILTClient::SetPolyGridTextureInfo"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetPolyGridInfo(HLOCALOBJ hObj, char **pBytes, uint32 *pWidth, uint32 *pHeight, PGColor **pColorTable) { LTSTUB("ILTClient::GetPolyGridInfo"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_FitPolyGrid(HLOCALOBJ hObj, LTVector *pMin, LTVector *pMax, LTVector *pPos, LTVector *pScale) { LTSTUB("ILTClient::FitPolyGrid"); return LT_ERROR; }
static void LTFnStub_ILTClient_GetLightColor(HLOCALOBJ hObj, float *r, float *g, float *b) { LTSTUB("ILTClient::GetLightColor"); }
static void LTFnStub_ILTClient_SetLightColor(HLOCALOBJ hObj, float r, float g, float b) { LTSTUB("ILTClient::SetLightColor"); }
static float LTFnStub_ILTClient_GetLightRadius(HLOCALOBJ hObj) { LTSTUB("ILTClient::GetLightRadius"); return 0.0f; }
static void LTFnStub_ILTClient_SetLightRadius(HLOCALOBJ hObj, float radius) { LTSTUB("ILTClient::SetLightRadius"); }
static LTRESULT LTFnStub_ILTClient_ClipSprite(HLOCALOBJ hObj, HPOLY hPoly) { LTSTUB("ILTClient::ClipSprite"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetNextModelNode(HLOCALOBJ hObject, HMODELNODE hNode, HMODELNODE *pNext) { LTSTUB("ILTClient::GetNextModelNode"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetModelNodeName(HLOCALOBJ hObject, HMODELNODE hNode, char *pName, uint32 maxLen) { LTSTUB("ILTClient::GetModelNodeName"); return LT_ERROR; }
static LTBOOL LTFnStub_ILTClient_GetSurfaceBounds(SurfaceData *pSurface, LTVector *pMin, LTVector *pMax) { LTSTUB("ILTClient::GetSurfaceBounds"); return LTFALSE; }
static LTRESULT LTFnStub_ILTClient_SetPolyHideStatus(HPOLY hPoly, LTBOOL bHide) { LTSTUB("ILTClient::SetPolyHideStatus"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetPolyTextureFlags(HPOLY hPoly, uint32 *pFlags) { LTSTUB("ILTClient::GetPolyTextureFlags"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_SetModelHook(ModelHookFn fn, void *pUser) { LTSTUB("ILTClient::SetModelHook"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetEngineHook(char *pName, void **pData) { LTSTUB("ILTClient::GetEngineHook"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_InitNetworking(char *pDriver, uint32 dwFlags) { LTSTUB("ILTClient::InitNetworking"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetServiceList(NetService* &pListHead) { LTSTUB("ILTClient::GetServiceList"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_FreeServiceList(NetService *pListHead) { LTSTUB("ILTClient::FreeServiceList"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_SelectService(HNETSERVICE hNetService) { LTSTUB("ILTClient::SelectService"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetSessionList(NetSession* &pListHead, char *pInfo) { LTSTUB("ILTClient::GetSessionList"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_FreeSessionList(NetSession *pListHead) { LTSTUB("ILTClient::FreeSessionList"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_AddInternetDriver() { LTSTUB("ILTClient::AddInternetDriver"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_RemoveInternetDriver() { LTSTUB("ILTClient::RemoveInternetDriver"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_IsLobbyLaunched(char* sDriver) { LTSTUB("ILTClient::IsLobbyLaunched"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetLobbyLaunchInfo(char* sDriver, void** ppLobbyLaunchData) { LTSTUB("ILTClient::GetLobbyLaunchInfo"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTClient_GetTcpIpAddress(char* sAddress, uint32 dwBufferSize) { LTSTUB("ILTClient::GetTcpIpAddress"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetGameInfo(void **ppData, uint32 *pLen) { LTSTUB("ILTServer::GetGameInfo"); return LT_ERROR; }
static HCLASS LTFnStub_ILTServer_GetClass(char *pName) { LTSTUB("ILTServer::GetClass"); return (HCLASS)0; }
static LTRESULT LTFnStub_ILTServer_GetStaticObject(HCLASS hClass, HOBJECT *obj) { LTSTUB("ILTServer::GetStaticObject"); return LT_ERROR; }
static HCLASS LTFnStub_ILTServer_GetObjectClass(HOBJECT hObject) { LTSTUB("ILTServer::GetObjectClass"); return (HCLASS)0; }
static LTBOOL LTFnStub_ILTServer_IsKindOf(HCLASS hClass, HCLASS hTest) { LTSTUB("ILTServer::IsKindOf"); return LTFALSE; }
static LPBASECLASS LTFnStub_ILTServer_CreateObject(HCLASS hClass, ObjectCreateStruct *pStruct) { LTSTUB("ILTServer::CreateObject"); return (LPBASECLASS)0; }
static LPBASECLASS LTFnStub_ILTServer_CreateObjectProps(HCLASS hClass, ObjectCreateStruct *pStruct, char *pszProps) { LTSTUB("ILTServer::CreateObjectProps"); return (LPBASECLASS)0; }
static uint32 LTFnStub_ILTServer_GetServerFlags() { LTSTUB("ILTServer::GetServerFlags"); return (uint32)0; }
static uint32 LTFnStub_ILTServer_SetServerFlags(uint32 flags) { LTSTUB("ILTServer::SetServerFlags"); return (uint32)0; }
static LTRESULT LTFnStub_ILTServer_CacheFile(uint32 fileType, char *pFilename) { LTSTUB("ILTServer::CacheFile"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetNextPoly(HPOLY *hPoly) { LTSTUB("ILTServer::GetNextPoly"); return LT_ERROR; }
static void LTFnStub_ILTServer_StartCounter(LTCounter *pCounter) { LTSTUB("ILTServer::StartCounter"); }
static uint32 LTFnStub_ILTServer_EndCounter(LTCounter *pCounter) { LTSTUB("ILTServer::EndCounter"); return (uint32)0; }
static LTFLOAT LTFnStub_ILTServer_Random(LTFLOAT min, LTFLOAT max) { LTSTUB("ILTServer::Random"); return 0.0f; }
static int LTFnStub_ILTServer_IntRandom(int min, int max) { LTSTUB("ILTServer::IntRandom"); return (int)0; }
static LTFLOAT LTFnStub_ILTServer_RandomScale(LTFLOAT scale) { LTSTUB("ILTServer::RandomScale"); return 0.0f; }
static void LTFnStub_ILTServer_BPrint(char *pMsg, ...) { LTSTUB("ILTServer::BPrint"); }
static void LTFnStub_ILTServer_DebugOut(char *pMsg, ...) { LTSTUB("ILTServer::DebugOut"); }
static LTRESULT LTFnStub_ILTServer_GetSkyDef(SkyDef *pDef) { LTSTUB("ILTServer::GetSkyDef"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_SetSkyDef(SkyDef *pDef) { LTSTUB("ILTServer::SetSkyDef"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_AddObjectToSky(HOBJECT hObj, uint32 index) { LTSTUB("ILTServer::AddObjectToSky"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_RemoveObjectFromSky(HOBJECT hObj) { LTSTUB("ILTServer::RemoveObjectFromSky"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetGlobalLightObject(HOBJECT *hObj) { LTSTUB("ILTServer::GetGlobalLightObject"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_SetGlobalLightObject(HOBJECT hObj) { LTSTUB("ILTServer::SetGlobalLightObject"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetPortalFlags(char *pPortalName, uint32 *pFlags) { LTSTUB("ILTServer::GetPortalFlags"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_SetPortalFlags(char *pPortalName, uint32 flags) { LTSTUB("ILTServer::SetPortalFlags"); return LT_ERROR; }
static LTBOOL LTFnStub_ILTServer_IntersectSegment(IntersectQuery *pQuery, IntersectInfo *pInfo) { LTSTUB("ILTServer::IntersectSegment"); return LTFALSE; }
static LTBOOL LTFnStub_ILTServer_CastRay(IntersectQuery *pQuery, IntersectInfo *pInfo) { LTSTUB("ILTServer::CastRay"); return LTFALSE; }
static ObjectList* LTFnStub_ILTServer_GetPointAreas(LTVector *pPoint) { LTSTUB("ILTServer::GetPointAreas"); return 0; }
static ObjectList* LTFnStub_ILTServer_GetBoxIntersecters(LTVector *pMin, LTVector *pMax) { LTSTUB("ILTServer::GetBoxIntersecters"); return 0; }
static LTBOOL LTFnStub_ILTServer_GetPointShade(LTVector *pPoint, LTVector *pColor) { LTSTUB("ILTServer::GetPointShade"); return LTFALSE; }
static ObjectList* LTFnStub_ILTServer_FindObjectsTouchingSphere(LTVector *pPosition, float radius) { LTSTUB("ILTServer::FindObjectsTouchingSphere"); return 0; }
static void LTFnStub_ILTServer_RelinquishList(ObjectList *pList) { LTSTUB("ILTServer::RelinquishList"); }
static ObjectList* LTFnStub_ILTServer_CreateObjectList() { LTSTUB("ILTServer::CreateObjectList"); return 0; }
static ObjectLink* LTFnStub_ILTServer_AddObjectToList(ObjectList *pList, HOBJECT hObj) { LTSTUB("ILTServer::AddObjectToList"); return 0; }
static void LTFnStub_ILTServer_RemoveObjectFromList(ObjectList *pList, HOBJECT hObj) { LTSTUB("ILTServer::RemoveObjectFromList"); }
static LTRESULT LTFnStub_ILTServer_AttachClient(HCLIENT hParent, HCLIENT hChild) { LTSTUB("ILTServer::AttachClient"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_DetachClient(HCLIENT hClient) { LTSTUB("ILTServer::DetachClient"); return LT_ERROR; }
static HCLIENT LTFnStub_ILTServer_GetNextClient(HCLIENT hPrev) { LTSTUB("ILTServer::GetNextClient"); return (HCLIENT)0; }
static HCLIENTREF LTFnStub_ILTServer_GetNextClientRef(HCLIENTREF hRef) { LTSTUB("ILTServer::GetNextClientRef"); return (HCLIENTREF)0; }
static uint32 LTFnStub_ILTServer_GetClientRefInfoFlags(HCLIENTREF hClient) { LTSTUB("ILTServer::GetClientRefInfoFlags"); return (uint32)0; }
static LTBOOL LTFnStub_ILTServer_GetClientRefName(HCLIENTREF hClient, char *pName, int maxLen) { LTSTUB("ILTServer::GetClientRefName"); return LTFALSE; }
static HOBJECT LTFnStub_ILTServer_GetClientRefObject(HCLIENTREF hClient) { LTSTUB("ILTServer::GetClientRefObject"); return (HOBJECT)0; }
static uint32 LTFnStub_ILTServer_GetClientID(HCLIENT hClient) { LTSTUB("ILTServer::GetClientID"); return (uint32)0; }
static LTBOOL LTFnStub_ILTServer_GetClientName(HCLIENT hClient, char *pName, int maxLen) { LTSTUB("ILTServer::GetClientName"); return LTFALSE; }
static void LTFnStub_ILTServer_SetClientInfoFlags(HCLIENT hClient, uint32 dwClientFlags) { LTSTUB("ILTServer::SetClientInfoFlags"); }
static uint32 LTFnStub_ILTServer_GetClientInfoFlags(HCLIENT hClient) { LTSTUB("ILTServer::GetClientInfoFlags"); return (uint32)0; }
static void LTFnStub_ILTServer_SetClientUserData(HCLIENT hClient, void *pData) { LTSTUB("ILTServer::SetClientUserData"); }
static void* LTFnStub_ILTServer_GetClientUserData(HCLIENT hClient) { LTSTUB("ILTServer::GetClientUserData"); return 0; }
static void LTFnStub_ILTServer_KickClient(HCLIENT hClient, uint32 nReasonCode) { LTSTUB("ILTServer::KickClient"); }
static LTRESULT LTFnStub_ILTServer_SetClientViewPos(HCLIENT hClient, LTVector *pPos) { LTSTUB("ILTServer::SetClientViewPos"); return LT_ERROR; }
static void LTFnStub_ILTServer_RunGameConString(char *pString) { LTSTUB("ILTServer::RunGameConString"); }
static void LTFnStub_ILTServer_SetGameConVar(char *pName, char *pVal) { LTSTUB("ILTServer::SetGameConVar"); }
static HCONVAR LTFnStub_ILTServer_GetGameConVar(char *pName) { LTSTUB("ILTServer::GetGameConVar"); return (HCONVAR)0; }
static LTRESULT LTFnStub_ILTServer_GetPropString(char *pPropName, char *pRet, int maxLen) { LTSTUB("ILTServer::GetPropString"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetPropVector(char *pPropName, LTVector *pRet) { LTSTUB("ILTServer::GetPropVector"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetPropColor(char *pPropName, LTVector *pRet) { LTSTUB("ILTServer::GetPropColor"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetPropReal(char *pPropName, float *pRet) { LTSTUB("ILTServer::GetPropReal"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetPropFlags(char *pPropName, uint32 *pRet) { LTSTUB("ILTServer::GetPropFlags"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetPropBool(char *pPropName, LTBOOL *pRet) { LTSTUB("ILTServer::GetPropBool"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetPropLongInt(char *pPropName, long *pRet) { LTSTUB("ILTServer::GetPropLongInt"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetPropRotation(char *pPropName, LTRotation *pRet) { LTSTUB("ILTServer::GetPropRotation"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetPropRotationEuler(char *pPropName, LTVector *pAngles) { LTSTUB("ILTServer::GetPropRotationEuler"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetPropGeneric(char *pPropName, GenericProp *pProp) { LTSTUB("ILTServer::GetPropGeneric"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_DoesPropExist(char *pPropName, int *pPropType) { LTSTUB("ILTServer::DoesPropExist"); return LT_ERROR; }
static HOBJECT LTFnStub_ILTServer_ObjectToHandle(LPBASECLASS pObject) { LTSTUB("ILTServer::ObjectToHandle"); return (HOBJECT)0; }
static LPBASECLASS LTFnStub_ILTServer_HandleToObject(HOBJECT hObject) { LTSTUB("ILTServer::HandleToObject"); return (LPBASECLASS)0; }
static LTBOOL LTFnStub_ILTServer_IsCommandOn(HCLIENT hClient, int command) { LTSTUB("ILTServer::IsCommandOn"); return LTFALSE; }
static LTBOOL LTFnStub_ILTServer_UpperStrcmp(char *pStr1, char *pStr2) { LTSTUB("ILTServer::UpperStrcmp"); return LTFALSE; }
static int LTFnStub_ILTServer_Parse(char *pCommand, char **pNewCommandPos, char *argBuffer, char **argPointers, int *nArgs) { LTSTUB("ILTServer::Parse"); return (int)0; }
static LTRESULT LTFnStub_ILTServer_PlaySound(PlaySoundInfo *pPlaySoundInfo) { LTSTUB("ILTServer::PlaySound"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetSoundDuration(HLTSOUND hSound, LTFLOAT *fDuration) { LTSTUB("ILTServer::GetSoundDuration"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_IsSoundDone(HLTSOUND hSound, LTBOOL *bDone) { LTSTUB("ILTServer::IsSoundDone"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_KillSound(HLTSOUND hSound) { LTSTUB("ILTServer::KillSound"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_KillSoundLoop(HLTSOUND hSound) { LTSTUB("ILTServer::KillSoundLoop"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_CreateInterObjectLink(HOBJECT hOwner, HOBJECT hLinked) { LTSTUB("ILTServer::CreateInterObjectLink"); return LT_ERROR; }
static void LTFnStub_ILTServer_BreakInterObjectLink(HOBJECT hOwner, HOBJECT hLinked) { LTSTUB("ILTServer::BreakInterObjectLink"); }
static LTRESULT LTFnStub_ILTServer_CreateAttachment(HOBJECT hParent, HOBJECT hChild, char *pSocketName, LTVector *pOffset, LTRotation *pRotationOffset, HATTACHMENT *pAttachment) { LTSTUB("ILTServer::CreateAttachment"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_RemoveAttachment(HATTACHMENT hAttachment) { LTSTUB("ILTServer::RemoveAttachment"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_FindAttachment(HOBJECT hParent, HOBJECT hChild, HATTACHMENT *hAttachment) { LTSTUB("ILTServer::FindAttachment"); return LT_ERROR; }
static LTBOOL LTFnStub_ILTServer_GetObjectColor(HOBJECT hObject, float *r, float *g, float *b, float *a) { LTSTUB("ILTServer::GetObjectColor"); return LTFALSE; }
static LTBOOL LTFnStub_ILTServer_SetObjectColor(HOBJECT hObject, float r, float g, float b, float a) { LTSTUB("ILTServer::SetObjectColor"); return LTFALSE; }
static uint32 LTFnStub_ILTServer_GetObjectUserFlags(HOBJECT hObj) { LTSTUB("ILTServer::GetObjectUserFlags"); return (uint32)0; }
static LTRESULT LTFnStub_ILTServer_SetObjectUserFlags(HOBJECT hObj, uint32 flags) { LTSTUB("ILTServer::SetObjectUserFlags"); return LT_ERROR; }
static HOBJECT LTFnStub_ILTServer_GetNextObject(HOBJECT hObj) { LTSTUB("ILTServer::GetNextObject"); return (HOBJECT)0; }
static HOBJECT LTFnStub_ILTServer_GetNextInactiveObject(HOBJECT hObj) { LTSTUB("ILTServer::GetNextInactiveObject"); return (HOBJECT)0; }
static void LTFnStub_ILTServer_SetNextUpdate(HOBJECT hObj, LTFLOAT nextUpdate) { LTSTUB("ILTServer::SetNextUpdate"); }
static void LTFnStub_ILTServer_SetDeactivationTime(HOBJECT hObj, LTFLOAT fDeactivationTime) { LTSTUB("ILTServer::SetDeactivationTime"); }
static void LTFnStub_ILTServer_PingObjects(HOBJECT hObj) { LTSTUB("ILTServer::PingObjects"); }
static void LTFnStub_ILTServer_GetObjectPos(HOBJECT hObj, LTVector *pos) { LTSTUB("ILTServer::GetObjectPos"); }
static void LTFnStub_ILTServer_SetObjectPos(HOBJECT hObj, LTVector *pos) { LTSTUB("ILTServer::SetObjectPos"); }
static void LTFnStub_ILTServer_ScaleObject(HOBJECT hObj, LTVector *pNewScale) { LTSTUB("ILTServer::ScaleObject"); }
static LTRESULT LTFnStub_ILTServer_GetObjectScale(HOBJECT hObj, LTVector *pScale) { LTSTUB("ILTServer::GetObjectScale"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_TeleportObject(HOBJECT hObj, LTVector *pNewPos) { LTSTUB("ILTServer::TeleportObject"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetLastCollision(CollisionInfo *pInfo) { LTSTUB("ILTServer::GetLastCollision"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetObjectRotation(HOBJECT hObj, LTRotation *pRotation) { LTSTUB("ILTServer::GetObjectRotation"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_SetObjectRotation(HOBJECT hObj, LTRotation *pRotation) { LTSTUB("ILTServer::SetObjectRotation"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_RotateObject(HOBJECT hObj, LTRotation *pRotation) { LTSTUB("ILTServer::RotateObject"); return LT_ERROR; }
static void LTFnStub_ILTServer_TiltToPlane(HOBJECT hObj, LTVector *pNormal) { LTSTUB("ILTServer::TiltToPlane"); }
static void LTFnStub_ILTServer_SetObjectState(HOBJECT hObj, int state) { LTSTUB("ILTServer::SetObjectState"); }
static int LTFnStub_ILTServer_GetObjectState(HOBJECT hObj) { LTSTUB("ILTServer::GetObjectState"); return (int)0; }
static void LTFnStub_ILTServer_SetBlockingPriority(HOBJECT hObj, uint8 pri) { LTSTUB("ILTServer::SetBlockingPriority"); }
static uint8 LTFnStub_ILTServer_GetBlockingPriority(HOBJECT hObj) { LTSTUB("ILTServer::GetBlockingPriority"); return (uint8)0; }
static LTRESULT LTFnStub_ILTServer_ClipSprite(HOBJECT hObj, HPOLY hPoly) { LTSTUB("ILTServer::ClipSprite"); return LT_ERROR; }
static void LTFnStub_ILTServer_GetLightColor(HOBJECT hObj, float *r, float *g, float *b) { LTSTUB("ILTServer::GetLightColor"); }
static void LTFnStub_ILTServer_SetLightColor(HOBJECT hObj, float r, float g, float b) { LTSTUB("ILTServer::SetLightColor"); }
static float LTFnStub_ILTServer_GetLightRadius(HOBJECT hObj) { LTSTUB("ILTServer::GetLightRadius"); return 0.0f; }
static void LTFnStub_ILTServer_SetLightRadius(HOBJECT hObj, float radius) { LTSTUB("ILTServer::SetLightRadius"); }
static LTRESULT LTFnStub_ILTServer_GetNextModelNode(HOBJECT hObject, HMODELNODE hNode, HMODELNODE *pNext) { LTSTUB("ILTServer::GetNextModelNode"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetModelNodeName(HOBJECT hObject, HMODELNODE hNode, char *pName, uint32 maxLen) { LTSTUB("ILTServer::GetModelNodeName"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetModelCommandString(HOBJECT hObj, char *pStr, uint32 maxLen) { LTSTUB("ILTServer::GetModelCommandString"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_SetModelNodeHideStatus(HOBJECT hObj, char *pNodeName, LTBOOL bHidden) { LTSTUB("ILTServer::SetModelNodeHideStatus"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetModelNodeHideStatus(HOBJECT hObj, char *pNodeName, LTBOOL *bHidden) { LTSTUB("ILTServer::GetModelNodeHideStatus"); return LT_ERROR; }
static const char * LTFnStub_ILTServer_GetAnimName(HOBJECT hObject, HMODELANIM hAnim) { LTSTUB("ILTServer::GetAnimName"); return 0; }
static void LTFnStub_ILTServer_SetModelPlaying(HOBJECT hObj, LTBOOL bPlaying) { LTSTUB("ILTServer::SetModelPlaying"); }
static LTBOOL LTFnStub_ILTServer_GetModelPlaying(HOBJECT hObj) { LTSTUB("ILTServer::GetModelPlaying"); return LTFALSE; }
static LTBOOL LTFnStub_ILTServer_GetModelFilenames(HOBJECT hObj, char *pFilename, int fileBufLen, char *pSkinName, int skinBufLen) { LTSTUB("ILTServer::GetModelFilenames"); return LTFALSE; }
static uint32 LTFnStub_ILTServer_GetObjectContainers(HOBJECT hObj, HOBJECT *pContainerList, uint32 *pFlagList, uint32 maxListSize) { LTSTUB("ILTServer::GetObjectContainers"); return (uint32)0; }
static uint32 LTFnStub_ILTServer_GetContainedObjects(HOBJECT hContainer, HOBJECT *pObjectList, uint32 *pFlagList, uint32 maxListSize) { LTSTUB("ILTServer::GetContainedObjects"); return (uint32)0; }
static LTRESULT LTFnStub_ILTServer_SaveObjects(char *pszSaveFileName, ObjectList *pList, uint32 dwParam, uint32 flags) { LTSTUB("ILTServer::SaveObjects"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_RestoreObjects(char *pszRestoreFileName, uint32 dwParam, uint32 flags) { LTSTUB("ILTServer::RestoreObjects"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_LoadWorld(char *pszWorldFileName, uint32 flags) { LTSTUB("ILTServer::LoadWorld"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_RunWorld() { LTSTUB("ILTServer::RunWorld"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_UpdateSessionName(const char* sName) { LTSTUB("ILTServer::UpdateSessionName"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetSessionName(char* sName, uint32 dwBufferSize) { LTSTUB("ILTServer::GetSessionName"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_SendToServerApp(char *pMsg, uint32 nLen) { LTSTUB("ILTServer::SendToServerApp"); return LT_ERROR; }
static LTRESULT LTFnStub_ILTServer_GetTcpIpAddress(char* sAddress, uint32 dwBufferSize, uint16 &hostPort) { LTSTUB("ILTServer::GetTcpIpAddress"); return LT_ERROR; }
static FileEntry* LTFnStub_ILTServer_GetFileList(char *pDirName) { LTSTUB("ILTServer::GetFileList"); return 0; }
static void LTFnStub_ILTServer_FreeFileList(FileEntry *pHead) { LTSTUB("ILTServer::FreeFileList"); }

// --- function-pointer members: wiring ------------------------------
inline void LTSkel_WireFnPtrs_ILTClient(ILTClient* p)
{
    p->StartGame = LTFnStub_ILTClient_StartGame;
    p->GetGameMode = LTFnStub_ILTClient_GetGameMode;
    p->GetLocalClientID = LTFnStub_ILTClient_GetLocalClientID;
    p->IsConnected = LTFnStub_ILTClient_IsConnected;
    p->Disconnect = LTFnStub_ILTClient_Disconnect;
    p->Shutdown = LTFnStub_ILTClient_Shutdown;
    p->ShutdownWithMessage = LTFnStub_ILTClient_ShutdownWithMessage;
    p->FlipScreen = LTFnStub_ILTClient_FlipScreen;
    p->ClearScreen = LTFnStub_ILTClient_ClearScreen;
    p->Start3D = LTFnStub_ILTClient_Start3D;
    p->RenderCamera = LTFnStub_ILTClient_RenderCamera;
    p->RenderObjects = LTFnStub_ILTClient_RenderObjects;
    p->StartOptimized2D = LTFnStub_ILTClient_StartOptimized2D;
    p->EndOptimized2D = LTFnStub_ILTClient_EndOptimized2D;
    p->SetOptimized2DBlend = LTFnStub_ILTClient_SetOptimized2DBlend;
    p->GetOptimized2DBlend = LTFnStub_ILTClient_GetOptimized2DBlend;
    p->SetOptimized2DColor = LTFnStub_ILTClient_SetOptimized2DColor;
    p->GetOptimized2DColor = LTFnStub_ILTClient_GetOptimized2DColor;
    p->End3D = LTFnStub_ILTClient_End3D;
    p->GetRenderModes = LTFnStub_ILTClient_GetRenderModes;
    p->RelinquishRenderModes = LTFnStub_ILTClient_RelinquishRenderModes;
    p->GetRenderMode = LTFnStub_ILTClient_GetRenderMode;
    p->SetRenderMode = LTFnStub_ILTClient_SetRenderMode;
    p->ShutdownRender = LTFnStub_ILTClient_ShutdownRender;
    p->GetFileList = LTFnStub_ILTClient_GetFileList;
    p->FreeFileList = LTFnStub_ILTClient_FreeFileList;
    p->GetWorldInfoString = LTFnStub_ILTClient_GetWorldInfoString;
    p->ReadConfigFile = LTFnStub_ILTClient_ReadConfigFile;
    p->WriteConfigFile = LTFnStub_ILTClient_WriteConfigFile;
    p->GetAxisOffsets = LTFnStub_ILTClient_GetAxisOffsets;
    p->PlayJoystickEffect = LTFnStub_ILTClient_PlayJoystickEffect;
    p->InitMusic = LTFnStub_ILTClient_InitMusic;
    p->SetMusicDirectory = LTFnStub_ILTClient_SetMusicDirectory;
    p->InitInstruments = LTFnStub_ILTClient_InitInstruments;
    p->LoadSong = LTFnStub_ILTClient_LoadSong;
    p->DestroyAllSongs = LTFnStub_ILTClient_DestroyAllSongs;
    p->PlayList = LTFnStub_ILTClient_PlayList;
    p->PlayBreak = LTFnStub_ILTClient_PlayBreak;
    p->StopMusic = LTFnStub_ILTClient_StopMusic;
    p->PauseMusic = LTFnStub_ILTClient_PauseMusic;
    p->ResumeMusic = LTFnStub_ILTClient_ResumeMusic;
    p->AddSongToPlayList = LTFnStub_ILTClient_AddSongToPlayList;
    p->DeletePlayList = LTFnStub_ILTClient_DeletePlayList;
    p->PlayMotif = LTFnStub_ILTClient_PlayMotif;
    p->StopMotif = LTFnStub_ILTClient_StopMotif;
    p->GetMusicVolume = LTFnStub_ILTClient_GetMusicVolume;
    p->SetMusicVolume = LTFnStub_ILTClient_SetMusicVolume;
    p->GetSound3DProviderLists = LTFnStub_ILTClient_GetSound3DProviderLists;
    p->ReleaseSound3DProviderList = LTFnStub_ILTClient_ReleaseSound3DProviderList;
    p->InitSound = LTFnStub_ILTClient_InitSound;
    p->GetSoundVolume = LTFnStub_ILTClient_GetSoundVolume;
    p->SetSoundVolume = LTFnStub_ILTClient_SetSoundVolume;
    p->SetReverbProperties = LTFnStub_ILTClient_SetReverbProperties;
    p->GetReverbProperties = LTFnStub_ILTClient_GetReverbProperties;
    p->PlaySound = LTFnStub_ILTClient_PlaySound;
    p->SetSoundPosition = LTFnStub_ILTClient_SetSoundPosition;
    p->GetSoundPosition = LTFnStub_ILTClient_GetSoundPosition;
    p->PauseSounds = LTFnStub_ILTClient_PauseSounds;
    p->ResumeSounds = LTFnStub_ILTClient_ResumeSounds;
    p->GetSoundDuration = LTFnStub_ILTClient_GetSoundDuration;
    p->GetSoundTimer = LTFnStub_ILTClient_GetSoundTimer;
    p->GetSoundData = LTFnStub_ILTClient_GetSoundData;
    p->GetSoundOffset = LTFnStub_ILTClient_GetSoundOffset;
    p->IsDone = LTFnStub_ILTClient_IsDone;
    p->KillSound = LTFnStub_ILTClient_KillSound;
    p->KillSoundLoop = LTFnStub_ILTClient_KillSoundLoop;
    p->SetListener = LTFnStub_ILTClient_SetListener;
    p->GetListener = LTFnStub_ILTClient_GetListener;
    p->IntersectSegment = LTFnStub_ILTClient_IntersectSegment;
    p->CastRay = LTFnStub_ILTClient_CastRay;
    p->FindObjectsInSphere = LTFnStub_ILTClient_FindObjectsInSphere;
    p->CreateFont = LTFnStub_ILTClient_CreateFont;
    p->DeleteFont = LTFnStub_ILTClient_DeleteFont;
    p->SetFontExtraSpace = LTFnStub_ILTClient_SetFontExtraSpace;
    p->GetFontExtraSpace = LTFnStub_ILTClient_GetFontExtraSpace;
    p->CreateColor = LTFnStub_ILTClient_CreateColor;
    p->DeleteColor = LTFnStub_ILTClient_DeleteColor;
    p->SetupColor1 = LTFnStub_ILTClient_SetupColor1;
    p->SetupColor2 = LTFnStub_ILTClient_SetupColor2;
    p->GetBorderSize = LTFnStub_ILTClient_GetBorderSize;
    p->OptimizeSurface = LTFnStub_ILTClient_OptimizeSurface;
    p->UnoptimizeSurface = LTFnStub_ILTClient_UnoptimizeSurface;
    p->GetScreenSurface = LTFnStub_ILTClient_GetScreenSurface;
    p->CreateSurfaceFromBitmap = LTFnStub_ILTClient_CreateSurfaceFromBitmap;
    p->CreateSurfaceFromString = LTFnStub_ILTClient_CreateSurfaceFromString;
    p->CreateSurface = LTFnStub_ILTClient_CreateSurface;
    p->DeleteSurface = LTFnStub_ILTClient_DeleteSurface;
    p->GetSurfaceUserData = LTFnStub_ILTClient_GetSurfaceUserData;
    p->SetSurfaceUserData = LTFnStub_ILTClient_SetSurfaceUserData;
    p->GetPixel = LTFnStub_ILTClient_GetPixel;
    p->SetPixel = LTFnStub_ILTClient_SetPixel;
    p->GetStringDimensions = LTFnStub_ILTClient_GetStringDimensions;
    p->DrawStringToSurface = LTFnStub_ILTClient_DrawStringToSurface;
    p->GetSurfaceDims = LTFnStub_ILTClient_GetSurfaceDims;
    p->DrawBitmapToSurface = LTFnStub_ILTClient_DrawBitmapToSurface;
    p->DrawSurfaceMasked = LTFnStub_ILTClient_DrawSurfaceMasked;
    p->DrawSurfaceSolidColor = LTFnStub_ILTClient_DrawSurfaceSolidColor;
    p->DrawSurfaceToSurface = LTFnStub_ILTClient_DrawSurfaceToSurface;
    p->DrawSurfaceToSurfaceTransparent = LTFnStub_ILTClient_DrawSurfaceToSurfaceTransparent;
    p->ScaleSurfaceToSurface = LTFnStub_ILTClient_ScaleSurfaceToSurface;
    p->ScaleSurfaceToSurfaceTransparent = LTFnStub_ILTClient_ScaleSurfaceToSurfaceTransparent;
    p->ScaleSurfaceToSurfaceSolidColor = LTFnStub_ILTClient_ScaleSurfaceToSurfaceSolidColor;
    p->WarpSurfaceToSurface = LTFnStub_ILTClient_WarpSurfaceToSurface;
    p->WarpSurfaceToSurfaceTransparent = LTFnStub_ILTClient_WarpSurfaceToSurfaceTransparent;
    p->WarpSurfaceToSurfaceSolidColor = LTFnStub_ILTClient_WarpSurfaceToSurfaceSolidColor;
    p->TransformSurfaceToSurface = LTFnStub_ILTClient_TransformSurfaceToSurface;
    p->TransformSurfaceToSurfaceTransparent = LTFnStub_ILTClient_TransformSurfaceToSurfaceTransparent;
    p->FillRect = LTFnStub_ILTClient_FillRect;
    p->GetSurfaceAlpha = LTFnStub_ILTClient_GetSurfaceAlpha;
    p->SetSurfaceAlpha = LTFnStub_ILTClient_SetSurfaceAlpha;
    p->RegisterConsoleProgram = LTFnStub_ILTClient_RegisterConsoleProgram;
    p->UnregisterConsoleProgram = LTFnStub_ILTClient_UnregisterConsoleProgram;
    p->GetConsoleVar = LTFnStub_ILTClient_GetConsoleVar;
    p->StartCounter = LTFnStub_ILTClient_StartCounter;
    p->EndCounter = LTFnStub_ILTClient_EndCounter;
    p->SetGlobalPanTexture = LTFnStub_ILTClient_SetGlobalPanTexture;
    p->SetGlobalPanInfo = LTFnStub_ILTClient_SetGlobalPanInfo;
    p->AddSurfaceEffect = LTFnStub_ILTClient_AddSurfaceEffect;
    p->SetInputState = LTFnStub_ILTClient_SetInputState;
    p->ClearInput = LTFnStub_ILTClient_ClearInput;
    p->GetDeviceBindings = LTFnStub_ILTClient_GetDeviceBindings;
    p->FreeDeviceBindings = LTFnStub_ILTClient_FreeDeviceBindings;
    p->StartDeviceTrack = LTFnStub_ILTClient_StartDeviceTrack;
    p->TrackDevice = LTFnStub_ILTClient_TrackDevice;
    p->EndDeviceTrack = LTFnStub_ILTClient_EndDeviceTrack;
    p->GetDeviceObjects = LTFnStub_ILTClient_GetDeviceObjects;
    p->FreeDeviceObjects = LTFnStub_ILTClient_FreeDeviceObjects;
    p->GetDeviceName = LTFnStub_ILTClient_GetDeviceName;
    p->IsDeviceEnabled = LTFnStub_ILTClient_IsDeviceEnabled;
    p->EnableDevice = LTFnStub_ILTClient_EnableDevice;
    p->GetGameTime = LTFnStub_ILTClient_GetGameTime;
    p->GetGameFrameTime = LTFnStub_ILTClient_GetGameFrameTime;
    p->DebugOut = LTFnStub_ILTClient_DebugOut;
    p->GetSkyDef = LTFnStub_ILTClient_GetSkyDef;
    p->IsCommandOn = LTFnStub_ILTClient_IsCommandOn;
    p->RunConsoleString = LTFnStub_ILTClient_RunConsoleString;
    p->GetClientObject = LTFnStub_ILTClient_GetClientObject;
    p->GetGlobalLightScale = LTFnStub_ILTClient_GetGlobalLightScale;
    p->SetGlobalLightScale = LTFnStub_ILTClient_SetGlobalLightScale;
    p->OffsetGlobalLightScale = LTFnStub_ILTClient_OffsetGlobalLightScale;
    p->Parse = LTFnStub_ILTClient_Parse;
    p->CreateObject = LTFnStub_ILTClient_CreateObject;
    p->GetObjectPos = LTFnStub_ILTClient_GetObjectPos;
    p->GetObjectRotation = LTFnStub_ILTClient_GetObjectRotation;
    p->SetObjectRotation = LTFnStub_ILTClient_SetObjectRotation;
    p->SetObjectPosAndRotation = LTFnStub_ILTClient_SetObjectPosAndRotation;
    p->GetObjectScale = LTFnStub_ILTClient_GetObjectScale;
    p->SetObjectScale = LTFnStub_ILTClient_SetObjectScale;
    p->GetObjectColor = LTFnStub_ILTClient_GetObjectColor;
    p->SetObjectColor = LTFnStub_ILTClient_SetObjectColor;
    p->GetObjectUserFlags = LTFnStub_ILTClient_GetObjectUserFlags;
    p->SetObjectUserFlags = LTFnStub_ILTClient_SetObjectUserFlags;
    p->GetObjectClientFlags = LTFnStub_ILTClient_GetObjectClientFlags;
    p->SetObjectClientFlags = LTFnStub_ILTClient_SetObjectClientFlags;
    p->GetObjectUserData = LTFnStub_ILTClient_GetObjectUserData;
    p->SetObjectUserData = LTFnStub_ILTClient_SetObjectUserData;
    p->Get3DCameraPt = LTFnStub_ILTClient_Get3DCameraPt;
    p->GetCameraFOV = LTFnStub_ILTClient_GetCameraFOV;
    p->SetCameraFOV = LTFnStub_ILTClient_SetCameraFOV;
    p->GetCameraRect = LTFnStub_ILTClient_GetCameraRect;
    p->SetCameraRect = LTFnStub_ILTClient_SetCameraRect;
    p->GetCameraLightAdd = LTFnStub_ILTClient_GetCameraLightAdd;
    p->SetCameraLightAdd = LTFnStub_ILTClient_SetCameraLightAdd;
    p->SetupParticleSystem = LTFnStub_ILTClient_SetupParticleSystem;
    p->SetSoftwarePSColor = LTFnStub_ILTClient_SetSoftwarePSColor;
    p->AddParticles = LTFnStub_ILTClient_AddParticles;
    p->GetParticles = LTFnStub_ILTClient_GetParticles;
    p->GetParticlePos = LTFnStub_ILTClient_GetParticlePos;
    p->SetParticlePos = LTFnStub_ILTClient_SetParticlePos;
    p->GetParticleLifetime = LTFnStub_ILTClient_GetParticleLifetime;
    p->GetParticleTotalLifetime = LTFnStub_ILTClient_GetParticleTotalLifetime;
    p->RemoveParticle = LTFnStub_ILTClient_RemoveParticle;
    p->OptimizeParticles = LTFnStub_ILTClient_OptimizeParticles;
    p->GetNextLine = LTFnStub_ILTClient_GetNextLine;
    p->GetLineInfo = LTFnStub_ILTClient_GetLineInfo;
    p->SetLineInfo = LTFnStub_ILTClient_SetLineInfo;
    p->AddLine = LTFnStub_ILTClient_AddLine;
    p->RemoveLine = LTFnStub_ILTClient_RemoveLine;
    p->SetupPolyGrid = LTFnStub_ILTClient_SetupPolyGrid;
    p->SetPolyGridTexture = LTFnStub_ILTClient_SetPolyGridTexture;
    p->SetPolyGridEnvMap = LTFnStub_ILTClient_SetPolyGridEnvMap;
    p->GetPolyGridTextureInfo = LTFnStub_ILTClient_GetPolyGridTextureInfo;
    p->SetPolyGridTextureInfo = LTFnStub_ILTClient_SetPolyGridTextureInfo;
    p->GetPolyGridInfo = LTFnStub_ILTClient_GetPolyGridInfo;
    p->FitPolyGrid = LTFnStub_ILTClient_FitPolyGrid;
    p->GetLightColor = LTFnStub_ILTClient_GetLightColor;
    p->SetLightColor = LTFnStub_ILTClient_SetLightColor;
    p->GetLightRadius = LTFnStub_ILTClient_GetLightRadius;
    p->SetLightRadius = LTFnStub_ILTClient_SetLightRadius;
    p->ClipSprite = LTFnStub_ILTClient_ClipSprite;
    p->GetNextModelNode = LTFnStub_ILTClient_GetNextModelNode;
    p->GetModelNodeName = LTFnStub_ILTClient_GetModelNodeName;
    p->GetSurfaceBounds = LTFnStub_ILTClient_GetSurfaceBounds;
    p->SetPolyHideStatus = LTFnStub_ILTClient_SetPolyHideStatus;
    p->GetPolyTextureFlags = LTFnStub_ILTClient_GetPolyTextureFlags;
    p->SetModelHook = LTFnStub_ILTClient_SetModelHook;
    p->GetEngineHook = LTFnStub_ILTClient_GetEngineHook;
    p->InitNetworking = LTFnStub_ILTClient_InitNetworking;
    p->GetServiceList = LTFnStub_ILTClient_GetServiceList;
    p->FreeServiceList = LTFnStub_ILTClient_FreeServiceList;
    p->SelectService = LTFnStub_ILTClient_SelectService;
    p->GetSessionList = LTFnStub_ILTClient_GetSessionList;
    p->FreeSessionList = LTFnStub_ILTClient_FreeSessionList;
    p->AddInternetDriver = LTFnStub_ILTClient_AddInternetDriver;
    p->RemoveInternetDriver = LTFnStub_ILTClient_RemoveInternetDriver;
    p->IsLobbyLaunched = LTFnStub_ILTClient_IsLobbyLaunched;
    p->GetLobbyLaunchInfo = LTFnStub_ILTClient_GetLobbyLaunchInfo;
    p->GetTcpIpAddress = LTFnStub_ILTClient_GetTcpIpAddress;
}

inline void LTSkel_WireFnPtrs_ILTServer(ILTServer* p)
{
    p->GetGameInfo = LTFnStub_ILTServer_GetGameInfo;
    p->GetClass = LTFnStub_ILTServer_GetClass;
    p->GetStaticObject = LTFnStub_ILTServer_GetStaticObject;
    p->GetObjectClass = LTFnStub_ILTServer_GetObjectClass;
    p->IsKindOf = LTFnStub_ILTServer_IsKindOf;
    p->CreateObject = LTFnStub_ILTServer_CreateObject;
    p->CreateObjectProps = LTFnStub_ILTServer_CreateObjectProps;
    p->GetServerFlags = LTFnStub_ILTServer_GetServerFlags;
    p->SetServerFlags = LTFnStub_ILTServer_SetServerFlags;
    p->CacheFile = LTFnStub_ILTServer_CacheFile;
    p->GetNextPoly = LTFnStub_ILTServer_GetNextPoly;
    p->StartCounter = LTFnStub_ILTServer_StartCounter;
    p->EndCounter = LTFnStub_ILTServer_EndCounter;
    p->Random = LTFnStub_ILTServer_Random;
    p->IntRandom = LTFnStub_ILTServer_IntRandom;
    p->RandomScale = LTFnStub_ILTServer_RandomScale;
    p->BPrint = LTFnStub_ILTServer_BPrint;
    p->DebugOut = LTFnStub_ILTServer_DebugOut;
    p->GetSkyDef = LTFnStub_ILTServer_GetSkyDef;
    p->SetSkyDef = LTFnStub_ILTServer_SetSkyDef;
    p->AddObjectToSky = LTFnStub_ILTServer_AddObjectToSky;
    p->RemoveObjectFromSky = LTFnStub_ILTServer_RemoveObjectFromSky;
    p->GetGlobalLightObject = LTFnStub_ILTServer_GetGlobalLightObject;
    p->SetGlobalLightObject = LTFnStub_ILTServer_SetGlobalLightObject;
    p->GetPortalFlags = LTFnStub_ILTServer_GetPortalFlags;
    p->SetPortalFlags = LTFnStub_ILTServer_SetPortalFlags;
    p->IntersectSegment = LTFnStub_ILTServer_IntersectSegment;
    p->CastRay = LTFnStub_ILTServer_CastRay;
    p->GetPointAreas = LTFnStub_ILTServer_GetPointAreas;
    p->GetBoxIntersecters = LTFnStub_ILTServer_GetBoxIntersecters;
    p->GetPointShade = LTFnStub_ILTServer_GetPointShade;
    p->FindObjectsTouchingSphere = LTFnStub_ILTServer_FindObjectsTouchingSphere;
    p->RelinquishList = LTFnStub_ILTServer_RelinquishList;
    p->CreateObjectList = LTFnStub_ILTServer_CreateObjectList;
    p->AddObjectToList = LTFnStub_ILTServer_AddObjectToList;
    p->RemoveObjectFromList = LTFnStub_ILTServer_RemoveObjectFromList;
    p->AttachClient = LTFnStub_ILTServer_AttachClient;
    p->DetachClient = LTFnStub_ILTServer_DetachClient;
    p->GetNextClient = LTFnStub_ILTServer_GetNextClient;
    p->GetNextClientRef = LTFnStub_ILTServer_GetNextClientRef;
    p->GetClientRefInfoFlags = LTFnStub_ILTServer_GetClientRefInfoFlags;
    p->GetClientRefName = LTFnStub_ILTServer_GetClientRefName;
    p->GetClientRefObject = LTFnStub_ILTServer_GetClientRefObject;
    p->GetClientID = LTFnStub_ILTServer_GetClientID;
    p->GetClientName = LTFnStub_ILTServer_GetClientName;
    p->SetClientInfoFlags = LTFnStub_ILTServer_SetClientInfoFlags;
    p->GetClientInfoFlags = LTFnStub_ILTServer_GetClientInfoFlags;
    p->SetClientUserData = LTFnStub_ILTServer_SetClientUserData;
    p->GetClientUserData = LTFnStub_ILTServer_GetClientUserData;
    p->KickClient = LTFnStub_ILTServer_KickClient;
    p->SetClientViewPos = LTFnStub_ILTServer_SetClientViewPos;
    p->RunGameConString = LTFnStub_ILTServer_RunGameConString;
    p->SetGameConVar = LTFnStub_ILTServer_SetGameConVar;
    p->GetGameConVar = LTFnStub_ILTServer_GetGameConVar;
    p->GetPropString = LTFnStub_ILTServer_GetPropString;
    p->GetPropVector = LTFnStub_ILTServer_GetPropVector;
    p->GetPropColor = LTFnStub_ILTServer_GetPropColor;
    p->GetPropReal = LTFnStub_ILTServer_GetPropReal;
    p->GetPropFlags = LTFnStub_ILTServer_GetPropFlags;
    p->GetPropBool = LTFnStub_ILTServer_GetPropBool;
    p->GetPropLongInt = LTFnStub_ILTServer_GetPropLongInt;
    p->GetPropRotation = LTFnStub_ILTServer_GetPropRotation;
    p->GetPropRotationEuler = LTFnStub_ILTServer_GetPropRotationEuler;
    p->GetPropGeneric = LTFnStub_ILTServer_GetPropGeneric;
    p->DoesPropExist = LTFnStub_ILTServer_DoesPropExist;
    p->ObjectToHandle = LTFnStub_ILTServer_ObjectToHandle;
    p->HandleToObject = LTFnStub_ILTServer_HandleToObject;
    p->IsCommandOn = LTFnStub_ILTServer_IsCommandOn;
    p->UpperStrcmp = LTFnStub_ILTServer_UpperStrcmp;
    p->Parse = LTFnStub_ILTServer_Parse;
    p->PlaySound = LTFnStub_ILTServer_PlaySound;
    p->GetSoundDuration = LTFnStub_ILTServer_GetSoundDuration;
    p->IsSoundDone = LTFnStub_ILTServer_IsSoundDone;
    p->KillSound = LTFnStub_ILTServer_KillSound;
    p->KillSoundLoop = LTFnStub_ILTServer_KillSoundLoop;
    p->CreateInterObjectLink = LTFnStub_ILTServer_CreateInterObjectLink;
    p->BreakInterObjectLink = LTFnStub_ILTServer_BreakInterObjectLink;
    p->CreateAttachment = LTFnStub_ILTServer_CreateAttachment;
    p->RemoveAttachment = LTFnStub_ILTServer_RemoveAttachment;
    p->FindAttachment = LTFnStub_ILTServer_FindAttachment;
    p->GetObjectColor = LTFnStub_ILTServer_GetObjectColor;
    p->SetObjectColor = LTFnStub_ILTServer_SetObjectColor;
    p->GetObjectUserFlags = LTFnStub_ILTServer_GetObjectUserFlags;
    p->SetObjectUserFlags = LTFnStub_ILTServer_SetObjectUserFlags;
    p->GetNextObject = LTFnStub_ILTServer_GetNextObject;
    p->GetNextInactiveObject = LTFnStub_ILTServer_GetNextInactiveObject;
    p->SetNextUpdate = LTFnStub_ILTServer_SetNextUpdate;
    p->SetDeactivationTime = LTFnStub_ILTServer_SetDeactivationTime;
    p->PingObjects = LTFnStub_ILTServer_PingObjects;
    p->GetObjectPos = LTFnStub_ILTServer_GetObjectPos;
    p->SetObjectPos = LTFnStub_ILTServer_SetObjectPos;
    p->ScaleObject = LTFnStub_ILTServer_ScaleObject;
    p->GetObjectScale = LTFnStub_ILTServer_GetObjectScale;
    p->TeleportObject = LTFnStub_ILTServer_TeleportObject;
    p->GetLastCollision = LTFnStub_ILTServer_GetLastCollision;
    p->GetObjectRotation = LTFnStub_ILTServer_GetObjectRotation;
    p->SetObjectRotation = LTFnStub_ILTServer_SetObjectRotation;
    p->RotateObject = LTFnStub_ILTServer_RotateObject;
    p->TiltToPlane = LTFnStub_ILTServer_TiltToPlane;
    p->SetObjectState = LTFnStub_ILTServer_SetObjectState;
    p->GetObjectState = LTFnStub_ILTServer_GetObjectState;
    p->SetBlockingPriority = LTFnStub_ILTServer_SetBlockingPriority;
    p->GetBlockingPriority = LTFnStub_ILTServer_GetBlockingPriority;
    p->ClipSprite = LTFnStub_ILTServer_ClipSprite;
    p->GetLightColor = LTFnStub_ILTServer_GetLightColor;
    p->SetLightColor = LTFnStub_ILTServer_SetLightColor;
    p->GetLightRadius = LTFnStub_ILTServer_GetLightRadius;
    p->SetLightRadius = LTFnStub_ILTServer_SetLightRadius;
    p->GetNextModelNode = LTFnStub_ILTServer_GetNextModelNode;
    p->GetModelNodeName = LTFnStub_ILTServer_GetModelNodeName;
    p->GetModelCommandString = LTFnStub_ILTServer_GetModelCommandString;
    p->SetModelNodeHideStatus = LTFnStub_ILTServer_SetModelNodeHideStatus;
    p->GetModelNodeHideStatus = LTFnStub_ILTServer_GetModelNodeHideStatus;
    p->GetAnimName = LTFnStub_ILTServer_GetAnimName;
    p->SetModelPlaying = LTFnStub_ILTServer_SetModelPlaying;
    p->GetModelPlaying = LTFnStub_ILTServer_GetModelPlaying;
    p->GetModelFilenames = LTFnStub_ILTServer_GetModelFilenames;
    p->GetObjectContainers = LTFnStub_ILTServer_GetObjectContainers;
    p->GetContainedObjects = LTFnStub_ILTServer_GetContainedObjects;
    p->SaveObjects = LTFnStub_ILTServer_SaveObjects;
    p->RestoreObjects = LTFnStub_ILTServer_RestoreObjects;
    p->LoadWorld = LTFnStub_ILTServer_LoadWorld;
    p->RunWorld = LTFnStub_ILTServer_RunWorld;
    p->UpdateSessionName = LTFnStub_ILTServer_UpdateSessionName;
    p->GetSessionName = LTFnStub_ILTServer_GetSessionName;
    p->SendToServerApp = LTFnStub_ILTServer_SendToServerApp;
    p->GetTcpIpAddress = LTFnStub_ILTServer_GetTcpIpAddress;
    p->GetFileList = LTFnStub_ILTServer_GetFileList;
    p->FreeFileList = LTFnStub_ILTServer_FreeFileList;
}
