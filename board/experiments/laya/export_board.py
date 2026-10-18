"""Export Laya (pinned thaitea/laya-vision) for the printed 16-button board.
Input: one crop centred on a single button (with margin), size LAYA_SIZE.
Outputs: probabilities [1,2] (not_pressed, pressed) and features [1, 2*d] = the head's per-option readout
vectors (input to the scorer), used for a scene-specific linear readout fitted on labeled frames."""
import os,json,sys
from pathlib import Path
import numpy as np, torch
from PIL import Image
import laya
from laya.vlm import build_vlm_inputs,collate_vlm,vlm_prefix,QTYPES
size=int(os.environ.get('LAYA_SIZE','128')); out=Path(sys.argv[1]); out.mkdir(parents=True,exist_ok=True)
torch.set_num_threads(8); torch.backends.mha.set_fastpath_enabled(False)
a=laya.load_vlm('thaitea/laya-vision',revision='f2fe3c12cb6d04c59d8a190250bf3fb40fc828dc',device='cpu',dtype='fp32',image_size=size)
question={'type':'noul','instructions':'Is a fingertip pressing the square button in the center of the image? Answer no if the finger is hovering above it, resting beside it, or absent.'}
im=Image.new('RGB',(size,size),(210,210,210))
state={'image':im,'note':'Top-down camera view of one printed button on a paper control board.'}
q=a._to_internal(question); prefix=vlm_prefix(a.processor,[im],a.prep)
row=build_vlm_inputs(a.processor,state,q,1024,256,prefix=prefix); row['qtype']=QTYPES[q['t']]
b=collate_vlm([row],a.processor.tokenizer.pad_token_id,with_pixels=False); pv=prefix['pixel_values'].unsqueeze(0)
print('tokens',b['input_ids'].shape,flush=True)
captured={}
a.model.scorer.register_forward_hook(lambda mod,inp,outp: captured.__setitem__('m',inp[0]))
class Fixed(torch.nn.Module):
    def __init__(self):
        super().__init__(); self.model=a.model
        for k in ['input_ids','attention_mask','marker_pos','marker_mask','qtype','option_span']: self.register_buffer(k,b[k])
        self.register_buffer('pam',prefix['pixel_attention_mask'].unsqueeze(0))
    def forward(self,pixels):
        logits,_=self.model(self.input_ids,self.attention_mask,self.marker_pos,self.marker_mask,self.qtype,pixel_values=pixels,pixel_attention_mask=self.pam,option_span=self.option_span)
        probs=torch.softmax(logits/a._checkpoint_temperature(QTYPES[q['t']],2),dim=-1)
        return probs, captured['m'].reshape(1,-1)
f=Fixed().eval()
with torch.no_grad():
    expected,feat=f(pv); original=a.predict(state,{'pressed':question})['answers']['pressed']['noul']
    print('baseline',expected.numpy(),'predict',original,'feat',feat.shape,flush=True)
    np.testing.assert_allclose(expected.numpy()[0,1],original,atol=1e-4)
    emb=f.model.encoder.vision_model.embeddings; cap=[]
    hk=emb.position_embedding.register_forward_pre_hook(lambda m,args: cap.append(args[0].detach().clone())); f(pv); hk.remove()
    class FixedEmb(torch.nn.Module):
        def __init__(self):
            super().__init__(); self.patch_embedding=emb.patch_embedding; self.position_embedding=emb.position_embedding; self.register_buffer('positions',cap[0])
        def forward(self,pixel_values,patch_attention_mask):
            return self.patch_embedding(pixel_values).flatten(2).transpose(1,2)+self.position_embedding(self.positions)
    f.model.encoder.vision_model.embeddings=FixedEmb()
    np.testing.assert_allclose(f(pv)[0].numpy(),expected.numpy(),atol=1e-6)
    torch.onnx.export(f,(pv,),str(out/'laya_board.onnx'),input_names=['pixels'],output_names=['probabilities','features'],opset_version=18,dynamo=False)
import onnx,onnxruntime as ort
from onnxruntime.quantization import quantize_dynamic,QuantType
m=onnx.load(str(out/'laya_board.onnx')); m.ir_version=min(m.ir_version,10); onnx.save(m,str(out/'laya_board.onnx'))
s=ort.InferenceSession(str(out/'laya_board.onnx'),providers=['CPUExecutionProvider'])
checks=[]
for seed in range(3):
    x=pv if seed==0 else torch.tensor(np.random.default_rng(seed).uniform(-1,1,pv.shape).astype('float32'))
    with torch.no_grad(): ref=f(x)[0].numpy()
    got=s.run(None,{'pixels':x.numpy()})[0]; np.testing.assert_allclose(got,ref,atol=1e-4); checks.append(got.tolist())
print('fp32 onnx validated',checks,flush=True)
quantize_dynamic(str(out/'laya_board.onnx'),str(out/'laya_board_int8.onnx'),weight_type=QuantType.QInt8,op_types_to_quantize=['MatMul','Gemm'],per_channel=True,extra_options={'MatMulConstBOnly':True})
s8=ort.InferenceSession(str(out/'laya_board_int8.onnx'),providers=['CPUExecutionProvider'])
print('int8',[s8.run(None,{'pixels':(pv if k==0 else torch.tensor(np.random.default_rng(k).uniform(-1,1,pv.shape).astype('float32'))).numpy()})[0].tolist() for k in range(3)],flush=True)
pv.numpy().astype('<f4').tofile(out/'test_pixels.bin')
json.dump({'question':question,'note':state['note'],'size':size,'tokens':int(b['input_ids'].shape[-1]),'fp32_checks':checks},open(out/'manifest.json','w'),indent=2)
print('done',flush=True)
