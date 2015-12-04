// RUN: %target-swift-frontend -primary-file %s -emit-ir -disable-objc-attr-requires-foundation-module | FileCheck %s

// REQUIRES: CPU=x86_64
// REQUIRES: objc_interop

// FIXME: These should be SIL tests, but we can't parse generic types in SIL
// yet.

@objc protocol ObjC {
  func method()
}

// CHECK-LABEL: define hidden void @_TF32sil_generic_witness_methods_objc16call_objc_method{{.*}}(%objc_object*, %swift.type* %T) {{.*}} {
// CHECK:         [[SEL:%.*]] = load i8*, i8** @"\01L_selector(method)", align 8
// CHECK:         [[CAST:%.*]] = bitcast %objc_object* %0 to [[SELFTYPE:%?.*]]*
// CHECK:         call void bitcast (void ()* @objc_msgSend to void ([[SELFTYPE]]*, i8*)*)([[SELFTYPE]]* [[CAST]], i8* [[SEL]])
func call_objc_method<T: ObjC>(x: T) {
  x.method()
}

// CHECK-LABEL: define hidden void @_TF32sil_generic_witness_methods_objc21call_call_objc_method{{.*}}(%objc_object*, %swift.type* %T) {{.*}} {
// CHECK:         call void @_TF32sil_generic_witness_methods_objc16call_objc_method{{.*}}(%objc_object* %0, %swift.type* %T)
func call_call_objc_method<T: ObjC>(x: T) {
  call_objc_method(x)
}

// CHECK-LABEL: define hidden void @_TF32sil_generic_witness_methods_objc28call_objc_existential_method{{.*}}(%objc_object*) {{.*}} {
// CHECK:         [[SEL:%.*]] = load i8*, i8** @"\01L_selector(method)", align 8
// CHECK:         [[CAST:%.*]] = bitcast %objc_object* %0 to [[SELFTYPE:%?.*]]*
// CHECK:         call void bitcast (void ()* @objc_msgSend to void ([[SELFTYPE]]*, i8*)*)([[SELFTYPE]]* [[CAST]], i8* [[SEL]])
func call_objc_existential_method(x: ObjC) {
  x.method()
}
