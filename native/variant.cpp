#include "stdafx.h"
#include "com4j.h"
#include "com4j_variant.h"
#include "xducer.h"
#include "unmarshaller.h"
#include "safearray.h"
#include "variant.h"

JNIEXPORT void JNICALL Java_com4j_Variant_clear0(JNIEnv* env, jclass, jobject image) {
	HRESULT hr = VariantClear((VARIANT*)env->GetDirectBufferAddress(image));
	if(FAILED(hr))
		error(env,__FILE__,__LINE__,hr,"failed to clear variant");
}

// change the variant type in the same place
void VariantChangeType( JNIEnv* env, VARIANT* v, VARTYPE type ) {
	VARIANT dst;
	VariantInit(&dst);
	HRESULT hr = VariantChangeType(&dst,v,0, type );
	if(FAILED(hr)) {
		error(env,__FILE__,__LINE__,hr,"failed to change the variant type");
		return;
	}
	VariantClear(v);
	*v = dst;
}

JNIEXPORT void JNICALL Java_com4j_Variant_changeType0(JNIEnv* env, jclass, jint type, jobject image) {
	VariantChangeType( env, (VARIANT*)env->GetDirectBufferAddress(image), (VARTYPE)type );
}

JNIEXPORT jobject JNICALL Java_com4j_Variant_convertTo(JNIEnv* env, jobject instance, jclass target) {
	jobject r = variantToObject(env,target,*com4jVariantToVARIANT(env,instance));
	if(r==reinterpret_cast<jobject>(-1)) {
		jstring name = javaLangClass_getName(env,target);
		error(env,__FILE__,__LINE__,E_FAIL,"Unable to convert to the %s",LPCSTR(JString(env,name)));
		return NULL;
	}
	return r;
}






class VariantHandler {
public:
	// returnss VARIANT allocated by 'new'
	virtual VARIANT* set( JNIEnv* env, jobject src ) = 0;
	virtual jobject get( JNIEnv* env, VARIANT* v, jclass retType ) = 0;
};

template <VARTYPE vt, class XDUCER>
class VariantHandlerImpl : public VariantHandler {
protected:
	inline typename XDUCER::NativeType& addr(VARIANT* v) {
		return *reinterpret_cast<XDUCER::NativeType*>(&v->boolVal);
	}
public:
	VARIANT* set( JNIEnv* env, jobject o ) {
		VARIANT* v = new VARIANT();
		VariantInit(v);
		v->vt = vt;
		addr(v) = XDUCER::toNative(
			env, static_cast<XDUCER::JavaType>(o) );
		return v;
	}
	jobject get( JNIEnv* env, VARIANT* v, jclass retType ) {
		_variant_t dst(v);
		dst.ChangeType(vt);
		jobject o = XDUCER::toJava(env, addr(&dst));
		return o;
	}
};

class ComObjectVariandHandlerImpl : public VariantHandlerImpl<VT_DISPATCH,xducer::Com4jObjectXducer> {
	typedef VariantHandlerImpl<VT_DISPATCH,xducer::Com4jObjectXducer> BASE;

	VARIANT* set( JNIEnv* env, jobject o) {
		VARIANT* v = BASE::set(env,o);
		IDispatch* pDisp = NULL;
		HRESULT hr = addr(v)->QueryInterface(&pDisp);
		if(SUCCEEDED(hr)) {
			// if possible, use VT_DISPATCH.
			addr(v)->Release();
			addr(v) = pDisp;
			v->vt = VT_DISPATCH;
		} // otherwise use VT_UNKNOWN. See java.net issue 2.
		return v;
	}
	jobject get( JNIEnv* env, VARIANT* v, jclass retType ) {
		jobject o = BASE::get(env,v,retType);

		// if the return type is an interface, use that to create a strongly typed object.
		// otherwise just return it as Com4jObject
		if(env->IsSameObject(retType,javaLangObject))
			return o;
		else
			return com4jWrapper_queryInterface(env,o,retType);
	}
};

class NoopVariantHandlerImpl : public VariantHandler {
public:
	VARIANT* set( JNIEnv* env, jobject src ) {
		VARIANT* pv = new VARIANT();
		VariantClear(pv);
		VariantCopy(pv, com4jVariantToVARIANT(env,src));
		return pv;
	}
	jobject get( JNIEnv* env, VARIANT* v, jclass retType ) {
		jobject r = com4j_Variant_new(env);
		::VariantCopy(com4jVariantToVARIANT(env,r),v);
		return r;
	}
};

// conversion table for variant
// from Java->native, we look for the cls field that can accept the current object,
// then if they match, we'll call the handler.
// from native->Java, we look for the vt field that can accept the current variant type.
// then if they match, we'll call the handler
struct SetterEntry {
	JClassID* cls;
	VARTYPE vt;
	VariantHandler* handler;
};

static SetterEntry setters[] = {
	{ &javaLangBoolean, VT_BOOL,		new VariantHandlerImpl<VT_BOOL,		xducer::BoxedVariantBoolXducer>() },
	{ &javaLangString,	VT_BSTR,		new VariantHandlerImpl<VT_BSTR,		xducer::StringXducer>() },
	{ &javaLangFloat,	VT_R4,			new VariantHandlerImpl<VT_R4,		xducer::BoxedFloatXducer>() },
	{ &javaLangDouble,	VT_R8,			new VariantHandlerImpl<VT_R8,		xducer::BoxedDoubleXducer>() },
	{ &javaLangShort,	VT_I2,			new VariantHandlerImpl<VT_I2,		xducer::BoxedShortXducer>() },
	{ &javaLangInteger,	VT_I4,			new VariantHandlerImpl<VT_I4,		xducer::BoxedIntXducer>() },
	{ &javaLangLong,	VT_I8,			new VariantHandlerImpl<VT_I8,		xducer::BoxedLongXducer>() },
	// see issue 2 on java.net. I used to convert a COM object to VT_UNKNOWN
	{ &com4j_Com4jObject,VT_DISPATCH,	new ComObjectVariandHandlerImpl() },
	{ &com4j_Com4jObject,VT_UNKNOWN,	new ComObjectVariandHandlerImpl() },
	{ &com4j_Variant,	0/*don't match from native->Java*/,		new NoopVariantHandlerImpl() },
	// TODO: Holder support
	{ NULL, 0, NULL }
};

VARIANT* convertToVariant( JNIEnv* env, jobject o ) {
	jclass cls = env->GetObjectClass(o);
	
	// consdier a conversion for scalars
	for( SetterEntry* p = setters; p->cls!=NULL; p++ ) {
		if( env->IsAssignableFrom( cls, *(p->cls) ) ) {
			VARIANT* v = p->handler->set(env,o);
			return v;
		}
	}

	// consider a conversion to SAFEARRAY
	pair<SAFEARRAY*,VARTYPE> sa = safearray::SafeArrayXducer::toNative2(env,static_cast<jarray>(o));
	if(sa.first!=NULL) {
		_variant_t* v = new _variant_t();
		v->vt = VT_ARRAY|sa.second;
		v->parray = sa.first;
		return v;
	}

	return NULL;
}

jobject variantToObject( JNIEnv* env, jclass retType, VARIANT& v ) {
	// return type driven
	for( SetterEntry* p = setters; p->cls!=NULL; p++ ) {
		if( env->IsAssignableFrom( retType, *(p->cls) ) ) {
			return p->handler->get(env,&v,retType);
		}
	}

	// if none is found, drive by the variant type
	for( SetterEntry* p = setters; p->cls!=NULL; p++ ) {
		if( v.vt==p->vt ) {
			return p->handler->get(env,&v,retType);
		}
	}

	// consider a conversion from SAFEARRAY
	if((v.vt&VT_ARRAY)!=0) {
		return safearray::SafeArrayXducer::toJava(env,v.parray);
	}

	// everything failed
	return reinterpret_cast<jobject>(-1);
}
