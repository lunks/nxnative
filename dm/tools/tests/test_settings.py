"""Offer the resolution control only when the rewritten build supports it, and the game's upscaler choice only to games whose shader map holds their own upscaler (agl's NDSR network: game.py upscaler_keys)."""
from pathlib import Path
import sys,tempfile,unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'games'))
import settings

class ResolutionSetting(unittest.TestCase):
    def test_verified_rewrite_required(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
            p=Path(tmp)/'main.fixups.tsv'
            for header,supported in [('# BUILD_ID\t3c10ec35c5ef4c3686b4053afb0e58e7\n',False),
                                     ('# DYNAMIC_RESOLUTION\t1\n',True),('',False)]:
                p.write_text(header)
                rows=settings.build('010056B015FE8000',tmp,'unused')['settings']
                dr=[s for s in rows if s['env']=='DM_DYNAMIC_RESOLUTION']
                self.assertEqual(bool(dr),supported)
                if dr:self.assertEqual((dr[0]['kind'],dr[0]['default']),('toggle','0'))
                fps=next(s for s in rows if s['env']=='DM_FPS_TARGET')
                self.assertEqual(fps['default'],'60')
                self.assertEqual(fps['options'],[['30','30 fps'],['60','60 fps']])

class FrameRateOption(unittest.TestCase):
    def test_120_only_on_the_mac_for_a_game_checked_at_that_speed(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
            (Path(tmp)/'main.fixups.tsv').write_text('')
            for gid,platform,has in [('04007EF01F8AE000','mac',True),('04007EF01F8AE000','ios',False),('010056B015FE8000','mac',False)]:   # Fortune's Weave (fps_max 120), NieR:Automata (none)
                with patch.dict(settings.os.environ,{'SETTINGS_PLATFORM':platform}):
                    fps=next(s for s in settings.build(gid,tmp,'unused')['settings'] if s['env']=='DM_FPS_TARGET')
                self.assertEqual(fps['default'],'60',gid)
                self.assertEqual(fps['options'],[['30','30 fps'],['60','60 fps']]+([['120','120 fps']] if has else []),(gid,platform))

class UpscalerSetting(unittest.TestCase):
    def test_only_for_games_naming_their_upscaler(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
            (Path(tmp)/'main.fixups.tsv').write_text('')
            for gid,shown in [('040074A01BF12000',True),('010056B015FE8000',False)]:   # Donkey Kong Bananza (its NDSR network), NieR:Automata (none)
                up=[s for s in settings.build(gid,tmp,'unused')['settings'] if s['env']=='DM_UPSCALER']
                self.assertEqual(bool(up),shown,gid)
                if up:self.assertEqual((up[0]['default'],up[0]['options']),('game',[['game',"Game's own"],['metalfx','MetalFX'],['metalfx-temporal','MetalFX temporal']]))
    def test_frame_interpolation_needs_the_camera_and_the_mac(self):   # DM_MFX_INTERP: an upscaler of the game's own and camera_mats (DM_GAME_MFX_FRAMES), on the Mac only
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
            (Path(tmp)/'main.fixups.tsv').write_text('')
            for gid,platform,shown in [('040074A01BF12000','mac',True),('040074A01BF12000','ios',False),('010056B015FE8000','mac',False)]:
                with patch.dict(settings.os.environ,{'SETTINGS_PLATFORM':platform}):
                    fi=[s for s in settings.build(gid,tmp,'unused')['settings'] if s['env']=='DM_MFX_INTERP']
                self.assertEqual(bool(fi),shown,(gid,platform))
                if fi:self.assertEqual((fi[0]['kind'],fi[0]['default']),('toggle','0'))
    def test_camera_jitter_needs_the_profile_and_the_temporal_scaler(self):   # DM_MFX_JITTER: the profile's camera_jitter and an upscaler of the game's own (DM_GAME_MFX_JITTER), drawn under MetalFX temporal
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
            (Path(tmp)/'main.fixups.tsv').write_text('')
            for gid,shown in [('040074A01BF12000',True),('010056B015FE8000',False)]:
                jt=[s for s in settings.build(gid,tmp,'unused')['settings'] if s['env']=='DM_MFX_JITTER']
                self.assertEqual(bool(jt),shown,gid)
                if jt:self.assertEqual((jt[0]['kind'],jt[0]['default'],jt[0]['needs']),('toggle','0','DM_UPSCALER=metalfx-temporal'))
    def test_single_upscale_needs_the_stub_and_the_mac(self):   # DM_SINGLE_UPSCALE: main's rewriter table has SINGLE_UPSCALE (Bananza's patches.py CAPABILITIES), on the Mac only
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
            p=Path(tmp)/'main.fixups.tsv'
            for header,platform,shown in [('# SINGLE_UPSCALE\t1\n','mac',True),('# SINGLE_UPSCALE\t1\n','ios',False),('','mac',False)]:
                p.write_text(header)
                with patch.dict(settings.os.environ,{'SETTINGS_PLATFORM':platform}):
                    su=[s for s in settings.build('040074A01BF12000',tmp,'unused')['settings'] if s['env']=='DM_SINGLE_UPSCALE']
                self.assertEqual(bool(su),shown,(header,platform))
                if su:self.assertEqual((su[0]['kind'],su[0]['default'],su[0]['section'],su[0].get('level'),su[0]['label'],[o[0] for o in su[0]['options']]),('choice','0','Resolution',None,'Output resolution',['0','1','display']))
                if su:self.assertEqual(su[0]['options'],[['0','Game default (smaller then stretched)'],['1','Window (2880x1620)'],['display','Match display']])

class SettingPatchRows(unittest.TestCase):   # patches:<VAR> and values patches: the profile's setting_patches.tsv, for the build being packed (main's BUILD_ID), on the Mac
    DKB,DKB_MAIN='040074A01BF12000','e25c90c1c136638885659fd55bf8a03b'
    def rows(self,gid,fixups,platform='mac'):
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']),patch.dict(settings.os.environ,{'SETTINGS_PLATFORM':platform}):
            (Path(tmp)/'main.fixups.tsv').write_text(fixups)
            return {s['env']:s for s in settings.build(gid,tmp,'unused')['settings'] if s['env'] in ('DM_SHADOW_RES','DM_SHADOW_MAP_SIZE','DM_UPSCALE_RATIO','DM_UPSCALE_DYNAMIC','DM_UPSCALE_SOFTEST','DM_EFFECTS_RES','DM_FPS_LOCK')}
    def test_bananza_shadow_rows(self):
        r=self.rows(self.DKB,f'# BUILD_ID\t{self.DKB_MAIN}\n')
        self.assertEqual([k for k in r if k.startswith('DM_SHADOW')],['DM_SHADOW_RES','DM_SHADOW_MAP_SIZE'])
        self.assertEqual((r['DM_SHADOW_RES']['kind'],r['DM_SHADOW_RES']['section'],r['DM_SHADOW_RES']['default']),('choice','Effects','game'))
        self.assertEqual(r['DM_SHADOW_RES']['options'],[['game','Game default'],['full','Full (no dynamic scaling)']])
        self.assertEqual((r['DM_SHADOW_MAP_SIZE']['default'],r['DM_SHADOW_MAP_SIZE']['options']),('game',[['game','Game default (2048)'],['3072','3072'],['4096','4096']]))
        self.assertIn('crash',r['DM_SHADOW_MAP_SIZE']['note'])
        self.assertNotIn('level',r['DM_SHADOW_RES'])   # advanced settings
    def test_bananza_resolution_model_rows(self):   # the Mac window's sections: Resolution in the picture's order, then Effects, Frame rate and HDR
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']),patch.dict(settings.os.environ,{'SETTINGS_PLATFORM':'mac','GFXAPI':'nvn'}):
            (Path(tmp)/'main.fixups.tsv').write_text(f'# BUILD_ID\t{self.DKB_MAIN}\n# SINGLE_UPSCALE\t1\n')
            listed=settings.build(self.DKB,tmp,'unused')['settings']
        sec={}
        for s in listed: sec.setdefault(s['section'],[]).append(s['env'])
        self.assertEqual(list(sec),['Game','Resolution','Effects','Frame rate','HDR','Controls','Performance'])
        want=['DM_SINGLE_UPSCALE','DM_UPSCALER','DM_UPSCALE_RATIO','DM_UPSCALE_DYNAMIC','DM_UPSCALE_SOFTEST','DM_PRESENT_UPSCALE']
        self.assertEqual([e for e in sec['Resolution'] if e in want],want,'in the picture\'s order (other rows, such as a MetalFX temporal option after the upscaler, may come between)')
        self.assertEqual(sec['Effects'],['DM_EFFECTS_RES','DM_SHADOW_RES','DM_SHADOW_MAP_SIZE'])
        self.assertEqual(sec['Frame rate'],['DM_FPS_LOCK','DM_MFX_INTERP'])
        self.assertEqual(sec['HDR'][0],'DM_HDR')
        self.assertEqual({s['section'] for s in listed if s.get('level')=='basic'},{'Game','Controls'},'Basic settings keep their sections')
        r=self.rows(self.DKB,f'# BUILD_ID\t{self.DKB_MAIN}\n')
        q=r['DM_UPSCALE_RATIO']
        self.assertEqual((q['kind'],q['default'],q['label'],q['section']),('choice','game','Render resolution','Resolution'))
        self.assertEqual([o[0] for o in q['options']],['game','1.0','1.5','1.6875','2.0','3.0'])
        self.assertEqual(q['options'][3][1],'Balanced (1.69)','an option is named by its label row, the write rows\' labels are the log\'s')
        self.assertEqual(q['conflicts'],'DM_UPSCALER=game:3.0','Ultra performance only with a MetalFX upscaler')
        base={'docked':[2304,1296],'handheld':[1920,1080],'when':[{'is':'DM_SINGLE_UPSCALE=1','docked':[2880,1620],'handheld':[1920,1080]},
                                                                  {'is':'DM_SINGLE_UPSCALE=display','docked':[0,0],'handheld':[1920,1080],'display':True}]}
        self.assertEqual(q['rate_base'],base,'the output sizes a rate divides (res_chain), for each option\'s render size; Match display\'s docked size is the screen\'s, filled in at launch')
        dyn=r['DM_UPSCALE_DYNAMIC']
        self.assertEqual((dyn['kind'],dyn['default'],dyn['needs']),('toggle','0','DM_UPSCALE_RATIO=1.0|1.5|1.6875|2.0'))
        lo=r['DM_UPSCALE_SOFTEST']
        self.assertEqual((lo['kind'],lo['default'],lo['label'],lo['needs'],lo['atleast'],lo['conflicts']),('choice','2.0','Softest','DM_UPSCALE_DYNAMIC=1','DM_UPSCALE_RATIO','DM_UPSCALER=game:3.0'))
        self.assertEqual([o[0] for o in lo['options']],['1.0','1.5','1.6875','2.0','3.0'])
        self.assertEqual(lo['rate_base'],base)
        self.assertEqual(r['DM_EFFECTS_RES']['options'],[['game','Game default (half)'],['full','Full (half- and quarter-res effects at the render size)']])
        self.assertFalse(any('level' in r[k] for k in r),'advanced settings')
    def test_bananza_rows_keep_the_iphone_sections(self):   # a row with a section per platform (Game|Resolution) stays under Game on the iPhone
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']),patch.dict(settings.os.environ,{'SETTINGS_PLATFORM':'ios','GFXAPI':'nvn'}):
            (Path(tmp)/'main.fixups.tsv').write_text(f'# BUILD_ID\t{self.DKB_MAIN}\n')
            listed=settings.build(self.DKB,tmp,'unused')['settings']
        self.assertEqual(list(dict.fromkeys(s['section'] for s in listed)),['Game','Controls','Performance'])
        self.assertEqual({s['env']:s['section'] for s in listed if s['env'] in ('DM_UPSCALER','DM_PRESENT_UPSCALE')},{'DM_UPSCALER':'Game','DM_PRESENT_UPSCALE':'Game'})
    def test_atleast_is_checked(self):
        for table,err in [('DM_A\tchoice\t1=A,2=B\t1\talways\tGame\tA\nDM_B\ttoggle\t-\t0\talways\tGame\tB\t\tatleast=DM_A\n','joins two choices'),
                          ('DM_A\tchoice\t1=A,2=B\t1\talways\tGame\tA\nDM_B\tchoice\t1=X,y=Y\t1\talways\tGame\tB\t\tatleast=DM_A\n','choice of numbers'),
                          ('DM_A\tchoice\t1=A,2=B\t1\talways\tGame\tA\nDM_B\tchoice\t1=X,2=Y\t1\talways\tGame\tB\t\tatleast=DM_C\n','no row sets'),
                          ('DM_A\tchoice\t1=A,2=B\t1\talways\tGame|Resolution|HDR\tA\n','platform section'),
                          ('DM_A\tchoice\t1=A,2=B\t1\talways\tGame|Pictures\tA\n','unknown section')]:
            with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
                (Path(tmp)/'settings.tsv').write_text(table)
                with patch.object(settings,'HERE',Path(tmp)),self.assertRaisesRegex(SystemExit,err):
                    settings.build('010056B015FE8000',tmp,'unused')
    def test_value_conflicts_and_value_lists_are_checked(self):
        cases=[('DM_A\tchoice\ta=A,b=B\ta\talways\tGame\tA\nDM_B\tchoice\tx=X,y=Y\tx\talways\tGame\tB\t\tconflicts=DM_A=a:z\n','never takes: z'),
               ('DM_A\tchoice\ta=A,b=B\ta\talways\tGame\tA\nDM_B\tchoice\tx=X,y=Y\tx\talways\tGame\tB\t\tconflicts=DM_A=a:x\n','cannot be the default'),
               ('DM_A\tchoice\ta=A,b=B\ta\talways\tGame\tA\nDM_B\ttoggle\t-\t0\talways\tGame\tB\t\tneeds=DM_A=a|c\n','never takes: c'),
               ('DM_A\tchoice\ta=A,b=B\ta\talways\tGame\tA\t\tneeds=DM_A=a\n','own variable')]
        for table,err in cases:
            with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
                (Path(tmp)/'settings.tsv').write_text(table)
                with patch.object(settings,'HERE',Path(tmp)),self.assertRaisesRegex(SystemExit,err):
                    settings.build('010056B015FE8000',tmp,'unused')
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
            (Path(tmp)/'settings.tsv').write_text('DM_A\tchoice\ta=A,b=B\ta\talways\tGame\tA\nDM_B\tchoice\tx=X,y=Y\tx\talways\tGame\tB\t\tconflicts=DM_A=a:y\n'
                                              'DM_C\ttoggle\t-\t0\talways\tGame\tC\t\tneeds=DM_A=a|b\n')
            with patch.object(settings,'HERE',Path(tmp)):
                rows={s['env']:s for s in settings.build('010056B015FE8000',tmp,'unused')['settings']}
        self.assertEqual((rows['DM_B']['conflicts'],rows['DM_C']['needs']),('DM_A=a:y','DM_A=a|b'))
    def test_bananza_frame_rate_mode(self):   # DM_FPS_LOCK: the frame-rate controller steps up as the game does; Prefer 60 tries 60 again after 120 frames and skips the veto, Hold 30 clamps at 2
        r=self.rows(self.DKB,f'# BUILD_ID\t{self.DKB_MAIN}\n')['DM_FPS_LOCK']
        self.assertEqual((r['kind'],r['section'],r['default']),('choice','Frame rate','game'))
        self.assertEqual(r['options'],[['game','Game (60 or 30)'],['60','Prefer 60'],['30','Hold 30']])
        self.assertIn('real speed',r['note'])
        self.assertNotIn('level',r)   # advanced settings
        rows=[l.split('\t') for l in (Path(settings.HERE)/self.DKB/'setting_patches.tsv').read_text().splitlines() if l.startswith('DM_FPS_LOCK\t') and len(l.split('\t'))>=8]
        self.assertEqual(sorted((v,o,a,b) for _,v,_,_,_,o,a,b in rows),[('30','0x204a88','913F4108','F947E908'),('30','0x204a8c','F9400108','5280004C'),('30','0x204ac0','6B08015F','6B0C015F'),('30','0x204ac4','1A89B10A','1A89B18A'),
                                                                         ('60','0x204b04','7A41B968','53017D68'),('60','0x204b08','5400006B','5100056C'),('60','0x204b0c','51000568','1A8CB108'),('60','0x204b90','35FFFC28','D503201F'),('60','0x204bc4','B940128C','52800F0C')])
        self.assertNotIn('0x204ab0',[row[5] for row in rows])   # the target itself is never fixed: a fixed target ran the game slow whenever frames took longer than it
        self.assertTrue(all(row[4]==self.DKB_MAIN for row in rows))   # main v458752 only
    def test_hidden_without_a_patch_for_the_build(self):
        for gid,fixups,platform in [(self.DKB,'# BUILD_ID\t'+'0'*32+'\n','mac'),(self.DKB,'','mac'),(self.DKB,f'# BUILD_ID\t{self.DKB_MAIN}\n','ios'),('010056B015FE8000','# BUILD_ID\t3c10ec35c5ef4c3686b4053afb0e58e7\n','mac')]:
            self.assertEqual(self.rows(gid,fixups,platform),{},(gid,fixups,platform))   # another build, no build ID, the iPhone (no code writes), a game without setting patches
    def test_patches_values_need_their_condition(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
            (Path(tmp)/'settings.tsv').write_text('DM_SHADOW_RES\tchoice\tpatches\tgame\talways\tGame\tShadow resolution\n')
            with patch.object(settings,'HERE',Path(tmp)),self.assertRaisesRegex(SystemExit,'values patches needs the condition patches:DM_SHADOW_RES'):
                settings.build('010056B015FE8000',tmp,'unused')

if __name__=='__main__':unittest.main()
